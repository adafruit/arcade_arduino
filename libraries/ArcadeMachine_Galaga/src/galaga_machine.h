// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// ArcadeMachine_Galaga: top-level Galaga machine state + lifecycle.
//
// The project's first MULTI-CPU machine (3x Z80: main/sub/sub2, sharing a
// common RAM map) -- a real step up from ArcadeMachine_Pacman's single-Z80
// hardware. Every hardware fact used across this library's files (memory
// map, interrupt/NMI scheme, CPU interleave quantum, custom I/O chain,
// tile/sprite/palette decode) was verified directly against MAME's own
// `galaga` driver source (src/mame/namco/galaga.cpp), the 06XX/51XX/54XX
// device sources (namco06.cpp/namco51.cpp/namco54.cpp), and the discrete
// audio netlist (galaga_a.cpp) -- all fetched and read directly this
// session, not recalled or guessed. See each .cpp file's own header
// comment for exact citations, and see project memory
// (galaga-port-research.md) for the full research trail and the
// HLE-over-LLE decision this library's 06xx/51xx/54xx files follow.
//
// Board-agnostic: talks only to ArcadeHAL and ArcadeCPU_Z80, never to a
// specific board's libraries.
//
// STATUS: verified working on real Fruit Jam hardware -- boots, passes its
// own power-on self-test (RAM march, grid, colour sweep), and plays, with
// starfield, sprites, joystick/fire input, WSG music and the 54XX explosion
// channel, at a flat 60fps with roughly 3ms of frame-budget headroom, run
// indefinitely (checked well past the 32-bit cycle-counter wrap that used to
// freeze it -- see galaga_machine.cpp's interleave_to_target()).
//
// All three video layers (starfield, sprites, tilemap), the Namco WSG's
// three voices and the 54XX explosion channel are implemented; see
// galaga_video.h, galaga_audio.h and galaga_54xx.h respectively.
//
// REMAINING GAPS, all deliberate and documented rather than oversights:
//  - The 54XX noise generator is an approximation, not a port: MAME runs the
//    chip's real MB8844 firmware, which we have no dump of. The envelope and
//    filter bands were tuned against a recording of the real board. See
//    galaga_54xx.h for exactly which parts are cited and which are by ear.
//  - 51XX credit/coinage bookkeeping is partial -- see galaga_51xx.h.
#ifndef GALAGA_MACHINE_H
#define GALAGA_MACHINE_H

#include <stdint.h>
#include <stdbool.h>
#include "z80.h"
#include "galaga_51xx.h"
#include "galaga_54xx.h"

#ifdef __cplusplus
extern "C" {
#endif

// Galaga's raw hardware framebuffer resolution, BEFORE the cabinet's
// physical 90-degree mount -- same role PACMAN_GAME_WIDTH/HEIGHT plays.
// Verified via MAME's galaga() machine_config: m_screen->set_raw(
// MASTER_CLOCK/3, 384, 0, 288, 264, 0, 224) -- visible area 288x224,
// byte-for-byte the same raster timing Pac-Man's hardware uses (same
// Namco video-generator family).
#define GALAGA_GAME_WIDTH  288
#define GALAGA_GAME_HEIGHT 224

// Memory region sizes, drawn from MAME's galaga_map() address map
// (src/mame/namco/galaga.cpp, all 3 CPUs share the same map above
// 0x4000) -- see galaga_ports.cpp's header comment for the exact address
// ranges each one backs and which CPU(s) can reach which.
#define GALAGA_ROM_MAIN_SIZE  0x4000 // main CPU program ROM, 0x0000-0x3FFF (4x4K chips)
#define GALAGA_ROM_SUB_SIZE   0x1000 // sub CPU program ROM,  0x0000-0x0FFF
#define GALAGA_ROM_SUB2_SIZE  0x1000 // sub2 CPU program ROM, 0x0000-0x0FFF
#define GALAGA_VIDEO_RAM_SIZE 0x0800 // shared, 0x8000-0x87FF -- tile numbers + sprite regs
#define GALAGA_RAM1_SIZE      0x0400 // shared, 0x8800-0x8BFF
#define GALAGA_RAM2_SIZE      0x0400 // shared, 0x9000-0x93FF
#define GALAGA_RAM3_SIZE      0x0400 // shared, 0x9800-0x9BFF

typedef struct {
    z80 cpu_main; // ArcadeCPU_Z80 instances -- read_byte/write_byte/port_in/
    z80 cpu_sub;  // port_out wired per-CPU by galaga_ports_wire(). All 3
    z80 cpu_sub2; // CPUs use the literal same galaga_map() in MAME's source
                  // for everything from 0x4000 up (only the 0x0000-0x3FFF
                  // ROM bank differs per CPU, via each CPU's own ROM
                  // region) -- i.e. WSG/misclatch/06xx/videolatch are
                  // addressable from all 3 CPUs at the bus level, even
                  // though in practice only main CPU's game code is
                  // expected to actually write to them. galaga_ports.cpp
                  // wires all 3 CPUs' read_byte/write_byte to the same
                  // shared >=0x4000 decode logic accordingly.

    uint8_t rom_main[GALAGA_ROM_MAIN_SIZE];
    uint8_t rom_sub[GALAGA_ROM_SUB_SIZE];
    uint8_t rom_sub2[GALAGA_ROM_SUB2_SIZE];

    uint8_t video_ram[GALAGA_VIDEO_RAM_SIZE];
    uint8_t ram1[GALAGA_RAM1_SIZE];
    uint8_t ram2[GALAGA_RAM2_SIZE];
    uint8_t ram3[GALAGA_RAM3_SIZE];

    // 06XX mux state (0x7000-0x70FF data, 0x7100 control -- addressable
    // from all 3 CPUs per the shared map, in practice only written by
    // main CPU's game code). Control register bits verified against namco06.cpp's
    // ctrl_w()/data_r()/data_w(): bits 0-3 = per-chip select (Galaga
    // wires only chip-select 0 -> 51xx and chip-select 3 -> 54xx, no
    // 50xx -- see galaga_ports.cpp), bit 4 = read(1)/write(0) mode,
    // bits 5-7 = NMI clock divider -- the periodic main-CPU NMI this
    // drives is implemented via io06_nmi_period/io06_nmi_next below.
    uint8_t io06_control;

    // 06XX control-register (0x7100) READ is a busy/ready STATUS, not an
    // echo of the byte written -- a real Phase A mistake, found by
    // cross-referencing danjulio/gcore_galagino's emulation.c (see
    // galaga_51xx.h's citation note): real reads return 0x00 while a
    // just-issued command is still "busy" and 0x10 once ready, gated by a
    // busy timer that reference implementation's own comment calls out
    // as "important for proper startup." That project tracks busy in
    // units of its own instruction-count interleave loop (5000 iterations
    // of 4 instructions each = ~20000 instructions); converted here to
    // main-CPU Z80 CYCLES (this project's own unit) at a rough ~5
    // cycles/instruction average -- an approximation of their
    // approximation, not an exact hardware timing, and the first thing to
    // re-tune if boot timing still looks wrong on real hardware.
    uint32_t namco_busy_until; // absolute cpu_main.cyc value; busy while cpu_main.cyc < this
    galaga_51xx_state io51;
    galaga_54xx_state io54;

    // 06XX periodic main-CPU NMI timer -- verified against namco06.cpp's
    // exact `ctrl_w_sync()`/`nmi_generate()` source (fetched and read
    // directly this session, superseding Phase A's original "not
    // implemented" note). Control bits 5-7 select a clock divider
    // (`divisor = 1 << ((control>>5)&7)`; `(control&0xE0)==0` stops the
    // timer entirely). The 06XX's own clock is `MASTER_CLOCK/6/64` =
    // 48000Hz (galaga()'s machine_config); converted to main-CPU Z80
    // cycles (3.072MHz) this is an exact `64 * divisor` cycles between
    // NMI pulses (3072000/48000 = 64) -- not an approximation.
    // Simplification vs real hardware: doesn't model the "first pulse
    // after entering read mode is suppressed" read-stretch behavior,
    // since this HLE's 51xx/54xx reads/writes complete instantly with no
    // real per-pulse latency to wait out.
    uint32_t io06_nmi_period; // 0 = timer stopped
    uint32_t io06_nmi_next;   // absolute cpu_main.cyc value of the next pulse

    // Counts main CPU PC hits at 0x34C9 (RET -- the RAM march test for the
    // current 1024-byte block PASSED) vs 0x34CA (checksum mismatch --
    // FAILED, enters the error-display handler). Originally scratch
    // instrumentation for the boot investigation; KEPT DELIBERATELY, and
    // now load-bearing in two places -- galaga_fruitjam.ino prints it in
    // the frame heartbeat, and tools/galaga_host's --stall detector uses
    // it as one of its two liveness signals. A healthy boot reaches
    // pass=7, fail=0. Do not remove without fixing both callers.
    uint32_t debug_checksum_pass;
    uint32_t debug_checksum_fail;

    // WSG voice registers (0x6800-0x681F writes -- namco_wsg_device::
    // pacman_sound_w, the SAME handler/register layout ArcadeMachine_
    // Pacman's pacman_audio.cpp already decodes at its own 0x5040-0x505F).
    // Consumed by galaga_audio.cpp's fill callback, which snapshots this
    // whole block under hal_audio_enter_critical() once per buffer.
    uint8_t wsg_regs[0x20];

    // "misclatch" 74LS259 outputs (0x6820-0x6827 writes, addressable from
    // all 3 CPUs per the shared map, one bit per address per the
    // ls259_device::write_d0 convention --
    // same convention pacman_system's interrupt_enable/sound_enable/etc.
    // fields use for Pac-Man's single mainlatch). Verified against
    // galaga()'s machine_config: q_out_cb<0/1/2> wiring below.
    bool irq1_enable;  // Q0 -- main CPU vblank IRQ enable (irq1_clear_w)
    bool irq2_enable;  // Q1 -- sub CPU vblank IRQ enable (irq2_clear_w)
    bool nmi2_enable;  // Q2 -- sub2 CPU NMI enable (nmion_w). Polarity
                        // confirmed against nmion_w's actual quoted body
                        // (`m_sub2_nmi_mask = !state`) -- galaga_ports.cpp
                        // applies that same inversion when writing this field.

    // Q3 -- sub/sub2 CPU RESET line, verified against galaga()'s
    // machine_config: `q_out_cb<3>().set_inputline("sub",
    // INPUT_LINE_RESET).invert()` (and the same for "sub2"). A REAL
    // hardware fact this project's first two debugging passes missed
    // entirely: sub and sub2 are HELD IN RESET (not executing at all)
    // from power-on until main CPU explicitly releases them by writing
    // this bit -- they do NOT run concurrently with main from boot the
    // way this library originally assumed ("both CPUs are already reset
    // at galaga_init() and this project doesn't model a live mid-game CPU
    // reset" -- true for later resets, but wrong for the boot-time
    // release gate, which is exactly what this field now models). Because
    // of the `.invert()` on the callback, the real reset-line state is
    // the OPPOSITE of the raw bit: bit=0 (the LS259's power-on-clear
    // default, matching this struct's memset-zero default) means HELD IN
    // RESET; bit=1 (main writes to 0x6823) means RELEASED, i.e. this
    // field name directly reflects "should sub/sub2 run" rather than
    // storing the raw inverted line state. Real hardware bug found
    // bringing Phase A up: without this gate, sub/sub2 ran from t=0
    // racing main for boot-time shared-RAM handshake bytes (0x9100/
    // 0x9101 in this ROM) in a completely different order than real
    // hardware ever produces, deadlocking all 3 CPUs in their own wait
    // loops permanently.
    bool sub_reset_released;

    // Absolute cpu_main.cyc value at the exact instant sub_reset_released
    // first became true -- captured once in galaga_ports.cpp's misclatch
    // write handler. interleave_to_target() uses this to reduce sub/sub2's
    // effective per-frame target ONLY on the specific frame release
    // happens mid-frame (by how many main-cycles had already elapsed in
    // that frame before release) -- without it, the frame's normal
    // cycles-since-frame-start `target` would ask sub/sub2 to "rush"
    // through an entire remaining frame's worth of cycles in one burst the
    // instant they're released, since their frame-start cyc snapshot
    // predates release. On every later frame this reduction is naturally
    // zero (release predates that frame's own start), so it's a one-frame
    // correction, not an ongoing cap -- an earlier version of this fix
    // instead capped sub/sub2's absolute cyc against main.cyc forever,
    // which over-corrected into a permanent starvation bug once sub caught
    // up to near-zero slack (confirmed via SWD). See project memory
    // (galaga-port-research.md) for the full investigation.
    uint32_t reset_release_main_cyc;

    // Sub2 CPU's NMI is NOT a single once-per-frame pulse the way this
    // library's fire_interrupts() originally modeled it -- cross-checked
    // against danjulio/gcore_galagino's emulate_frame() (a working
    // reference boot), which fires it TWICE per frame at specific
    // mid-frame points (its own comment: "run cpu2 nmi at ~line 64 and
    // line 192", i.e. real scanline-compare trigger circuitry, not a
    // single vblank-adjacent pulse). Converted from their instruction-
    // count quarter/three-quarter-of-frame positions to this project's
    // cycle-count equivalent (GALAGA_CYCLES_PER_FRAME/4 and *3/4) in
    // galaga_machine.cpp. These two bools latch whether each of the
    // current frame's two pulses has already fired, reset at the top of
    // each frame -- found while investigating a 3-CPU boot handshake
    // deadlock (see project memory).
    bool nmi2_fired_a;
    bool nmi2_fired_b;

    // DIP switches (DSWA/DSWB), read via `bosco_dsw_r` at 0x6800-0x6807
    // (galaga_ports.cpp) -- verified against the actual quoted
    // galaga_map()/bosco_dsw_r() source: each address 0x6800+offset
    // returns bit `offset` of DSWB in bit 0 and bit `offset` of DSWA in
    // bit 1 (`bit0 | (bit1<<1)`), a bit-serial read convention, NOT a
    // flat byte read the way Pac-Man's dsw1 field works. Defaults set by
    // galaga_load_rom() per MAME's own INPUT_PORTS_START(galaga) -- an
    // earlier version of galaga_ports.cpp returned a constant 0xFF for
    // this whole read range instead of implementing the real formula
    // (Phase A's first real hardware bug, found via a stuck-at-self-test
    // "OK" screen on actual hardware), which reads every DIP bit back as
    // set regardless of the real switch position -- see galaga_ports.cpp's
    // header comment for exactly which settings that corrupted (Bonus_Life,
    // Lives) and which it didn't (several defaults happen to be bit=1).
    uint8_t dswa, dswb;

    // "videolatch" 74LS259 outputs (0xA000-0xA007 writes, addressable
    // from all 3 CPUs per the shared map). Verified against galaga()'s
    // machine_config: q_out_cb<7> =
    // flip_screen_set; Q0-Q5 go to the STARFIELD_05XX device (comment in
    // the source itself: "Q0-Q5 to 05XX for starfield control"). Q6 has no
    // wiring found in the source read this session.
    bool    flip_screen;      // Q7
    // Q0-Q5 packed into the low 6 bits: Q2..Q0 = scroll speed index,
    // Q3 and Q4|2 select the two active star sets, Q5 = _STARCLR (enable).
    // Consumed by galaga_video.cpp's star_begin_frame().
    uint8_t starfield_control;

    // IN0/IN1 shadow bytes, updated once per frame by galaga_input_update()
    // before galaga_run_frame() executes any CPU cycles -- same convention
    // pacman_system's in0/in1 fields use. Bit layout verified against
    // MAME's INPUT_PORTS_START(galaga): IN0 bit1/bit3 = joystick
    // right/left (2-way -- this cabinet has no up/down), IN1 bit0 =
    // button1/fire, bit2/bit3 = start1/start2, bit4/bit5 = coin1/coin2,
    // bit6 = service1. All active-low. Cocktail-mode bits (IN0 bit5/bit7,
    // IN1 bit1) are left permanently inactive, same upright-only
    // convention ArcadeMachine_Pacman uses.
    uint8_t in0, in1;

    uint8_t rotation; // 0=landscape 1=90 CCW 2=180 3=90 CW (portrait, the
                      // default -- see galaga_init() for why it is 3 and
                      // not 1, which is what Invaders/LunarRescue use)
    bool    mirror_x; // horizontal mirror toggle (Pepper's-Ghost cabinets)
} galaga_system;

// Boot-error screen colors (RGB565) -- same convention as every other
// ArcadeMachine_*'s COLOR_ERROR_* constants.
#define GALAGA_COLOR_ERROR_NO_CARD   0xF800u // red    -- storage missing or won't mount
#define GALAGA_COLOR_ERROR_NO_ASSETS 0xFFE0u // yellow -- mounted, but ROM/PROM files missing

// Sets game-state defaults, wires all 3 Z80 cores' callbacks (see
// galaga_ports.h), and initializes video (hal_video_init()). Does not
// touch storage.
void galaga_init(galaga_system *system);

// Loads ROM/PROM assets via ArcadeHAL's storage contract (see
// galaga_assets.h), builds the tile/sprite/palette decode caches (see
// galaga_video.h). On success returns true; on failure returns false and
// sets *out_error_color to one of the GALAGA_COLOR_ERROR_* constants
// above. Also brings up audio (hal_audio_init + galaga_audio_init) once
// the assets it needs -- notably the WSG waveform PROM -- are loaded.
bool galaga_load_assets(galaga_system *system, uint16_t *out_error_color);

// Runs exactly one video frame: interleaves all 3 CPUs' execution in
// small time slices (approximating MAME's real ~512-Z80-cycle scheduler
// quantum -- see galaga_machine.cpp for the exact derivation and why
// Pac-Man's single-CPU "run the whole frame, then interrupt once" loop
// shape does not apply here), fires the vblank IRQ on main/sub (if
// enabled) and NMI on sub2 (if enabled) once per frame, then renders via
// ArcadeHAL's video contract. Call this in a tight loop from the sketch
// after galaga_input_update() has updated `system` for the frame.
void galaga_run_frame(galaga_system *system);

// Frame-budget diagnostics: peak single-scanline render time, and the
// longest run of consecutive non-blocking scanline acquires (>= the DVI
// queue depth of 8 means Core 1 starved -- a red line). Reading clears both.
void galaga_debug_take_starvation(uint32_t *render_max_us, uint32_t *noblock_run_max);

#ifdef __cplusplus
}
#endif

#endif
