// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// ArcadeMachine_BTime: top-level Burger Time machine state + lifecycle.
//
// Built from scratch against the real arcade ROM dump (btime_assets/rom/,
// the `btime` PARENT set, all 15 CRC32s verified against MAME's
// ROM_START( btime )) and against MAME's own driver source
// (src/mame/dataeast/btime.cpp and src/mame/dataeast/decocpu7.cpp, upstream
// mamedev/mame), fetched and read rather than recalled. Each file below
// cites the specific MAME construct its facts came from, and
// ../../../BTIME_PORT_PLAN.md is the long-form version of that research.
//
// FOUR THINGS MAKE THIS MACHINE UNLIKE EVERY OTHER GAME IN THIS PROJECT:
//
//  1. THERE IS NO VBLANK INTERRUPT. Not "we don't use it" -- the board does
//     not have one. The driver header says so outright: "These games don't
//     have VBLANK interrupts, but instead an IRQ or NMI (depending on the
//     particular board) is generated when a coin is inserted." The program
//     finds the beam by POLLING a vblank bit that the schematics wire into
//     a DIP-switch port (0x4003 bit 7). A port that returns a constant
//     there hangs the game in a wait loop -- see btime_ports.cpp, and see
//     DEVNOTES.md problem #24 for the last time an "unused" input bit here
//     turned out to be load-bearing.
//  2. THE MAIN CPU'S OPCODES ARE ENCRYPTED, and statefully. A DECO CPU-7
//     descrambles an instruction fetch if, and only if, a memory write has
//     happened since the previous fetch AND the fetch address satisfies
//     (pc & 0x104) == 0x104. So unlike Ms. Pac-Man's decode, this ROM
//     CANNOT be transformed once at load time; it lives in the fetch path
//     forever. See btime_ports.cpp's btime_read_opcode().
//  3. THE PALETTE IS RAM, not a PROM. This board has no colour PROM at all
//     (MAME: "Burger Time doesn't have a color PROM. It uses RAM to
//     dynamically create the palette."). 16 bytes at 0x0C00, which the game
//     writes. See btime_video.cpp.
//  4. TWO 6502s, sharing nothing but one 8-bit latch. Simpler than Galaga's
//     three-Z80 shared-RAM arrangement, but with three separate interrupt
//     mechanisms that are each easy to get subtly wrong -- a level-triggered
//     IRQ (latch written), an EDGE-triggered NMI (a scanline timer), and a
//     software enable for that NMI which starts OFF. See btime_run_frame().
//
// Board-agnostic: talks only to ArcadeHAL and ArcadeCPU_M6502, never to a
// specific board's libraries.
#ifndef BTIME_MACHINE_H
#define BTIME_MACHINE_H

#include <stdint.h>
#include <stdbool.h>
#include "m6502.h"

#ifdef __cplusplus
extern "C" {
#endif

// Burger Time's raw video geometry, BEFORE the cabinet's physical mount.
// From btime()'s screen configuration in btime.cpp:
//     m_screen->set_raw(12_MHz_XTAL / 2, 384, 8, 248, 272, 8, 248);
// i.e. a 6MHz dot clock, 384 dots per line of which 8..247 are visible, and
// 272 lines of which 8..247 are visible: a 240x240 VISIBLE window inside a
// 256x256 addressable raster.
//
// The 8-pixel offset on both axes is not cosmetic. MAME's draw routines
// work in raw 0..255 coordinates and let the cliprect do the cropping, so
// every coordinate formula ported into btime_video.cpp is in raw space and
// the visible window has to be applied on output. Getting this wrong shifts
// the whole picture by 8 pixels in both axes, which looks like a rendering
// bug in the tile addressing rather than a cropping mistake.
#define BTIME_GAME_WIDTH   240 // visible, raw x 8..247
#define BTIME_GAME_HEIGHT  240 // visible, raw y 8..247
#define BTIME_RASTER       256 // addressable, both axes

// Frame/scanline geometry, same source.
#define BTIME_SCANLINES_PER_FRAME 272
#define BTIME_FIRST_VISIBLE_LINE  8   // raw y of the first visible scanline
// 240 visible lines -> exactly HAL_VIDEO_SCANLINES_PER_FRAME submissions.

// Cycles per scanline, derived rather than measured, because on this board
// CPU and video both divide down from the same 12MHz crystal and come out
// to exact integers:
//
//   dot clock  = 12MHz / 2                 =  6,000,000
//   main CPU   = 12MHz / 2 / 2 / 2         =  1,500,000  = dot clock / 4
//   sound CPU  = 12MHz / 2 / 2 / 3 / 2     =    500,000  = dot clock / 12
//   frame      = 6,000,000 / (384 * 272)   =  57.4449 Hz
//
//   main  cycles/scanline = 384 / 4  = 96      (x272 = 26,112 per frame)
//   sound cycles/scanline = 384 / 12 = 32      (x272 =  8,704 per frame)
//
// From DECO_CPU7(config, m_maincpu, 12_MHz_XTAL / 2 / 2 / 2) and
// M6502(config, m_audiocpu, 12_MHz_XTAL / 2 / 2 / 3 / 2) in btime().
// (Guru's notes in the same driver header measured 57.4358 Hz / 15.6235 kHz
// on a real PCB; the difference from the figures above is the crystal, not
// a modelling choice.)
#define BTIME_MAIN_CYCLES_PER_LINE  96
#define BTIME_SOUND_CYCLES_PER_LINE 32

// Memory sizes, from btime_map() and audio_map() in btime.cpp.
#define BTIME_ROM_SIZE        0x5000 // 0xB000-0xFFFF mapped as ROM; the
                                     // `btime` set populates 0xC000-0xFFFF
#define BTIME_ROM_BASE        0xB000
#define BTIME_RAM_SIZE        0x0800 // 0x0000-0x07FF
#define BTIME_VIDEORAM_SIZE   0x0400 // 0x1000-0x13FF (char codes + sprites)
#define BTIME_COLORRAM_SIZE   0x0400 // 0x1400-0x17FF (char code bits 8-9)
#define BTIME_PALETTERAM_SIZE 0x0010 // 0x0C00-0x0C0F
#define BTIME_AUDIO_ROM_SIZE  0x1000 // 0xE000-0xEFFF, mirrored at 0xF000
#define BTIME_AUDIO_RAM_SIZE  0x0400 // 0x0000-0x03FF, mirrored through 0x1FFF

typedef struct {
    // Two 6502s. `cpu` is the DECO CPU-7 (encrypted fetches; its
    // read_opcode hook is wired in btime_ports_wire()); `audiocpu` is a
    // plain M6502 running ab14.12h.
    m6502 cpu;
    m6502 audiocpu;

    uint8_t rom[BTIME_ROM_SIZE];
    uint8_t ram[BTIME_RAM_SIZE];
    uint8_t videoram[BTIME_VIDEORAM_SIZE];
    uint8_t colorram[BTIME_COLORRAM_SIZE];
    uint8_t paletteram[BTIME_PALETTERAM_SIZE];

    uint8_t audio_rom[BTIME_AUDIO_ROM_SIZE];
    uint8_t audio_ram[BTIME_AUDIO_RAM_SIZE];

    // --- main CPU video latches ---
    // 0x4002 bit 0, via btime_video_control_w(): "Bit 0 = Flip screen,
    // Bit 1-7 = Unknown". Unconditional on this game (the DIP-gated variant
    // in the same driver, bnj_video_control_w, belongs to Bump 'n' Jump).
    bool flip_screen;
    // 0x4004, via bnj_scroll_w<0>(). Bit 4 enables the background layer,
    // bits 0-1 are its coarse 256-pixel scroll and bit 2 its page bank --
    // see btime_video.cpp. btime never writes bnj_scroll[1], so the fine
    // scroll byte its sibling games use is permanently 0 here.
    uint8_t bnj_scroll0;

    // --- input shadow bytes, refreshed once per frame ---
    // ALL ACTIVE LOW except SYSTEM's two coin bits -- see btime_input.cpp.
    uint8_t p1, p2, system_in, dsw1, dsw2;

    // --- video timing, the thing this machine polls instead of an IRQ ---
    uint16_t scanline; // 0..271, advanced by btime_run_frame()
    bool     vblank;   // true outside raw lines 8..247; read at 0x4003 bit 7

    // --- main CPU -> sound CPU ---
    uint8_t soundlatch;    // written at 0x4003, read at 0xA000
    bool    sound_irq;     // LEVEL: set by the write, cleared by the read
    bool    audio_nmi_en;  // 0xC000 bit 0; starts CLEAR (the ROM enables it)
    bool    audio_nmi_prev;// previous merged NMI line, for edge detection

    // --- main CPU IRQ (coin only; there is no vblank IRQ) ---
    bool coin_irq_pending; // one-shot, mirroring MAME's HOLD_LINE

    // --- DECO CPU-7 ---
    // "has a write happened since the last opcode fetch". Set by every main
    // CPU write, tested and cleared by every main CPU opcode fetch.
    bool had_written;

    uint8_t rotation; // 0=landscape 1=90 CCW 2=180 3=90 CW
    bool    mirror_x; // horizontal mirror toggle (Pepper's Ghost cabinets)
} btime_system;

// Boot-error screen colors (RGB565) -- same convention as every other
// machine library here.
#define BTIME_COLOR_ERROR_NO_CARD   0xF800u // red    -- storage missing
#define BTIME_COLOR_ERROR_NO_ASSETS 0xFFE0u // yellow -- mounted, ROMs missing

// Sets game-state defaults, wires both CPU cores' callbacks (see
// btime_ports.h), and initializes video (hal_video_init()). Does not touch
// storage.
void btime_init(btime_system *system);

// Loads ROM assets via ArcadeHAL's storage contract (see btime_assets.h),
// builds video's lookup tables, and brings up audio. On success returns
// true; on failure returns false and sets *out_error_color.
bool btime_load_assets(btime_system *system, uint16_t *out_error_color);

// Runs exactly one video frame: 272 scanlines' worth of both CPUs,
// interleaved with scanline output, with the vblank flag and the sound
// CPU's NMI timer advanced per scanline. Call in a tight loop from the
// sketch after btime_input_update().
void btime_run_frame(btime_system *system);

// DEBUG counters, for the questions this machine's failure modes can only
// be answered with a number rather than an opinion (DEVNOTES.md #32). All
// are cheap increments on paths the emulation itself does not read, and
// btime_debug_take_counters() resets everything except illegal_ops.
typedef struct {
    // Reads of 0x4003. If this is ZERO the machine is not polling video
    // timing, which for THIS game means it is not running at all -- there
    // is no vblank interrupt to fall back on.
    uint32_t vblank_reads;
    // Instruction fetches the DECO CPU-7 rule actually descrambled. Zero
    // means either the mask/flag logic is wrong or the case is genuinely
    // never reached, and those look identical from outside.
    uint32_t opcode_swaps;
    // Reads of the X/Y-swapped video/colour RAM windows (0x1800-0x1FFF).
    // The port plan lists "does anything READ these, or only write them" as
    // an open question; this answers it.
    uint32_t mirror_reads;
    // Undefined opcodes executed by either CPU, from ArcadeCPU_M6502's own
    // counter. A RUNNING TOTAL, not a delta -- the core never resets it.
    // Should stay 0.
    uint32_t illegal_ops;
    // Main CPU writes to the sound latch at 0x4003, i.e. how many commands
    // it actually asked the sound board for. Distinguishes "the sound CPU
    // is broken" from "nothing ever asked it for a sound".
    uint32_t latch_writes;
    // Interrupts actually delivered to the sound CPU. The NMI is the music
    // sequencer's clock (~976 Hz when enabled) and the IRQ is a command
    // arriving, so a silent machine with zero NMIs and a silent machine
    // with 16,000 NMIs are entirely different problems.
    uint32_t sound_nmis;
    uint32_t sound_irqs;
    // AY-3-8910 register writes by the sound CPU. If this is zero, no
    // amount of waveform code would make a sound.
    uint32_t ay_reg_writes;
    // Reads of the SYSTEM port at 0x4002 (start buttons, tilt, coins), and
    // coin interrupts actually DELIVERED to the main CPU. Together these
    // separate "the coin never reached the machine" from "the machine saw
    // the coin and declined it".
    uint32_t system_reads;
    uint32_t main_irqs;
    // Scanlines on which the main CPU had interrupts UNMASKED. On this
    // board the coin IRQ is the only interrupt, raised with MAME's
    // HOLD_LINE, so it can only be taken during one of these -- if the
    // count is zero, no coin can ever be accepted.
    uint32_t main_irq_windows;
} btime_counters;

void btime_debug_take_counters(const btime_system *system,
                               btime_counters *out);

#ifdef __cplusplus
}
#endif

#endif
