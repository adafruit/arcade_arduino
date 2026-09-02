// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// ArcadeMachine_DKong: top-level Donkey Kong machine state + lifecycle.
//
// Built from scratch against the real arcade ROM/PROM dump
// (dkong_assets/rom/) and verified against MAME's own `dkong` driver source
// (src/mame/nintendo/dkong.cpp and dkong_v.cpp, plus
// src/devices/machine/i8257.cpp and src/emu/video/resnet.cpp, upstream
// mamedev/mame), fetched and read rather than recalled. Each file below
// cites the specific MAME construct its facts came from.
//
// THREE THINGS MAKE THIS MACHINE UNLIKE THE PROJECT'S OTHER Z80 GAMES, and
// each has bitten somewhere in this project before in a different costume:
//
//  1. SPRITES ARRIVE BY DMA. The Z80 never writes sprite RAM directly. It
//     programs an i8257 DMA controller at 0x7800-0x780F and pulses 0x7D85;
//     the 8257 then copies the frame's sprite list into sprite RAM. Nothing
//     appears on screen at all without it. See dkong_ports.cpp.
//  2. INPUTS ARE ACTIVE HIGH. Every other game in this project is active
//     low (a released button reads 1). Here a released button reads 0, so
//     the memset-zero default is the correct idle state rather than "every
//     button held", which is the exact inverse of the trap called out in
//     tools/host_common/hal_host.cpp for Galaga. See dkong_input.cpp.
//  3. THE INTERRUPT IS AN NMI, not an IM0/IM1 IRQ -- fired at vblank and
//     gated by a software-writable mask at 0x7D84. A game that never
//     enables the mask simply never runs; see dkong_run_frame().
//
// Board-agnostic: talks only to ArcadeHAL and ArcadeCPU_Z80, never to a
// specific board's libraries.
#ifndef DKONG_MACHINE_H
#define DKONG_MACHINE_H

#include <stdint.h>
#include <stdbool.h>
#include "z80.h"

#ifdef __cplusplus
extern "C" {
#endif

// Donkey Kong's raw hardware framebuffer resolution, BEFORE the cabinet's
// physical 90-degree mount. From MAME's dkong.h video timing constants
// (HBEND=0, HBSTART=256, VBEND=16, VBSTART=240) as used by dkong_base()'s
// m_screen->set_raw(PIXEL_CLOCK, HTOTAL, HBEND, HBSTART, VTOTAL, VBEND,
// VBSTART): the visible area is 256 across by 224 down.
#define DKONG_GAME_WIDTH  256
#define DKONG_GAME_HEIGHT 224

// Memory region sizes, from MAME's dkong_map() (src/mame/nintendo/dkong.cpp).
#define DKONG_ROM_SIZE        0x5000 // 4x 4K program ROMs, 0x0000-0x4FFF
                                     // (only 0x4000 is populated by the
                                     // `dkong` set; 0x4000-0x4FFF is mapped
                                     // as ROM but empty here)
#define DKONG_WORK_RAM_SIZE   0x0C00 // general RAM,   0x6000-0x6BFF
#define DKONG_SPRITE_RAM_SIZE 0x0400 // sprite list,   0x7000-0x73FF (DMA target)
#define DKONG_VIDEO_RAM_SIZE  0x0400 // tile numbers,  0x7400-0x77FF

// i8257 DMA controller state. Deliberately NOT a general 8257 model -- see
// dkong_ports.cpp's dma_run() for exactly which subset this is and why.
typedef struct {
    uint16_t address[4]; // per-channel DMA address register
    uint16_t count[4];   // per-channel terminal count register (14 bits)
    uint8_t  mode[4];    // per-channel transfer mode, 2 bits (see dkong_ports.cpp)
    uint8_t  enable;     // channel-enable mask, register 8
    uint8_t  latch;      // the byte in flight between channel 0 and channel 1
    bool     msb;        // address/count registers take low byte then high byte
} dkong_dma;

typedef struct {
    z80 cpu; // ArcadeCPU_Z80 -- registers/flags/cycle count; read_byte/
             // write_byte/port_in/port_out wired by dkong_ports_wire().

    uint8_t rom[DKONG_ROM_SIZE];
    uint8_t work_ram[DKONG_WORK_RAM_SIZE];
    uint8_t sprite_ram[DKONG_SPRITE_RAM_SIZE];
    uint8_t video_ram[DKONG_VIDEO_RAM_SIZE];

    dkong_dma dma;

    // Video control latches, all written through the 0x7D80-0x7D87 block.
    // From dkong_flipscreen_w(), dkong_spritebank_w(), dkong_palettebank_w()
    // and nmi_mask_w() in dkong.cpp/dkong_v.cpp.
    bool    flip_screen;   // 0x7D82
    uint8_t sprite_bank;   // 0x7D83, 1 bit -- selects which half of sprite RAM is live
    uint8_t palette_bank;  // 0x7D86-0x7D87, 2 bits, set/cleared one bit per address
    bool    nmi_mask;      // 0x7D84 -- gates the vblank NMI

    // IN0/IN1/IN2/DSW0 shadow bytes. IN0/IN1/IN2 are updated once per frame
    // by dkong_input_update(); DSW0 is fixed at load time. ACTIVE HIGH --
    // see this file's header and dkong_input.cpp.
    uint8_t in0, in1, in2, dsw0;

    uint8_t rotation; // 0=landscape 1=90 CCW 2=180 3=90 CW
    bool    mirror_x; // horizontal mirror toggle (Pepper's Ghost cabinets)
} dkong_system;

// Boot-error screen colors (RGB565) -- same convention as every other
// machine library here.
#define DKONG_COLOR_ERROR_NO_CARD   0xF800u // red    -- storage missing or won't mount
#define DKONG_COLOR_ERROR_NO_ASSETS 0xFFE0u // yellow -- mounted, but ROM/PROM files missing

// Sets game-state defaults, wires the Z80 core's callbacks (see
// dkong_ports.h), and initializes video (hal_video_init()). Does not touch
// storage.
void dkong_init(dkong_system *system);

// Loads ROM/PROM assets via ArcadeHAL's storage contract (see
// dkong_assets.h), builds the tile/sprite/palette decode caches (see
// dkong_video.h), and brings up (silent) audio. On success returns true; on
// failure returns false and sets *out_error_color.
bool dkong_load_assets(dkong_system *system, uint16_t *out_error_color);

// Runs exactly one video frame: executes this frame's Z80 cycles
// interleaved with scanline output, and fires the vblank NMI if nmi_mask is
// set. Call in a tight loop from the sketch after dkong_input_update().
void dkong_run_frame(dkong_system *system);

// DEBUG: total DMA transfers and bytes moved since the last call, then
// resets both. The host harness uses this to answer "is the 8257 actually
// running" with a number -- if it is not, the screen has a background and
// no sprites, which is a subtler failure than a crash. Zero cost when
// unused; see tools/dkong_host/main.cpp.
// Mean microseconds per frame spent in dkong_audio_run_frame() over the
// last 60 frames -- the sound half's real cost against the 16660us budget.
uint32_t dkong_debug_audio_us(void);

void dkong_debug_take_dma_stats(uint32_t *out_transfers, uint32_t *out_bytes);

#ifdef __cplusplus
}
#endif

#endif
