// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// ArcadeMachine_LunarRescue: top-level Lunar Rescue machine state + lifecycle.
// Board-agnostic: talks only to ArcadeHAL and ArcadeCPU_i8080, never to a
// specific board's libraries. Structurally a sibling of
// ArcadeMachine_Invaders -- Lunar Rescue (Taito, 1979) is a different game
// on the same "8080bw" hardware family as Space Invaders (same CPU speed,
// same interrupt scheme, same 256x224 1bpp video RAM layout), but with a
// non-consecutive ROM map, real per-block hardware color instead of a
// monochrome+overlay screen, and a different sound-trigger wiring.
//
// Every hardware fact asserted in this library's comments was verified
// against MAME's current midw8080 driver sources rather than assumed by
// analogy to Invaders -- see the specific files cited below:
//   midw8080/mw8080bw.cpp  (base main_map, shared CPU clock/interrupt config)
//   midw8080/8080bw.cpp    (lrescue_io_map, lrescue ROM_START, INPUT_PORTS)
//   midw8080/8080bw_a.cpp  (lrescue_sh_port_1_w / lrescue_sh_port_2_w, sample names)
//   midw8080/8080bw_v.cpp  (screen_update_invadpt2 -- color PROM indexing)
//   emu/emupal.cpp         (palette_init_3bit_rbg -- color PROM -> RGB mapping)
#ifndef LRESCUE_MACHINE_H
#define LRESCUE_MACHINE_H

#include <stdint.h>
#include <stdbool.h>
#include "i8080.h"

#ifdef __cplusplus
extern "C" {
#endif

// Native Lunar Rescue video RAM resolution -- identical to Space Invaders'
// (same video generator hardware, same screen_update_invadpt2() bitmap
// layout in MAME; confirmed by ROM_LOAD addresses placing RAM at the same
// 0x2000-0x3fff range -- see lrescue_assets.h).
#define LRESCUE_GAME_WIDTH  256
#define LRESCUE_GAME_HEIGHT 224

// The i8080's real clock rate on this board family (same value
// invaders_machine.cpp's CYCLES_PER_FRAME derives from) -- exposed here so
// anything that needs to convert a cycle count to real elapsed time (e.g.
// lrescue_audio.cpp's cycle-timestamped speaker-bitstream reconstruction)
// uses the same constant rather than a second hardcoded copy of it.
#define LRESCUE_CPU_HZ 1996800.0

// Size of the color-map PROM (7643-1.cpu, moved to /prom/ -- see
// lrescue_assets.h) that lrescue_video.cpp indexes per 8x8-pixel block
// instead of Invaders' fixed white-on-black.
#define LRESCUE_COLOR_PROM_SIZE 0x400

typedef struct {
    Cpu_state state; // CPU state (registers, sp, pc, memory, etc.)

    int left;
    int right;
    int shot; // single fire/action button (Lunar Rescue's controls are a
              // 2-way joystick + one button, not Invaders' 3-button layout)
    int start1;
    int start2;
    int coin;
    int tilt;

    uint16_t ext_shift_data;   // External shift register (shift data) -- same
    uint8_t  ext_shift_offset; // mb14241 shifter wiring as Space Invaders.

    // IN2 bits 0-1 (lives) and bit 2 (tilt, live input) are meaningful;
    // bits 3 and 7 are present only for bit-position parity with
    // ArcadeMachine_Invaders' dip_switches[] -- MAME's own INPUT_PORTS_START
    // (lrescue) marks those two DIPUNUSED/factory-fixed (bonus life fixed at
    // 1500, "coin info" fixed on), so lrescue_load_rom() hardcodes them and
    // there is no in-game way to change them. Indices 4-6 are unused (P2
    // controls on a cocktail cabinet; this port targets upright only).
    uint8_t dip_switches[8];

    // Set by port 3 writes (lrescue_sh_port_1_w bit 2 in MAME) -- when true,
    // the whole screen renders in the "screen red" fore-color instead of the
    // per-block color PROM lookup (used for a warning/death flash effect).
    bool screen_red;

    // Set by port 5 writes bit 5, gated by cabinet type on real hardware
    // (upright: always 0). Tracked for parity with
    // ArcadeMachine_Invaders' cocktail_vertical_screen_flip but, like that
    // field, not currently consulted by the renderer.
    uint8_t flip_screen;

    // Raw per-8x8-pixel-block color-select PROM contents (3 bits used per
    // entry after masking &0x07; see lrescue_video.cpp for the exact index
    // formula, derived and numerically verified against MAME's
    // screen_update_invadpt2()). Loaded from /prom/7643-1.cpp by
    // lrescue_load_rom(); all-zero (-> palette index 0) if that file is
    // missing, which is a silent visual-only degradation, not a boot error.
    uint8_t color_prom[LRESCUE_COLOR_PROM_SIZE];

    uint8_t rotation;  // 0=landscape  1=90 deg CCW  2=180 deg  3=90 deg CW
    bool    mirror_x;  // horizontal mirror toggle (e.g. for a Pepper's Ghost cabinet)

    // Monotonic count of i8080 cycles executed since lrescue_init(), updated
    // by lrescue_run_frame(). This is a LOGICAL clock, not a wall-clock one:
    // a whole frame's cycles execute in a real-time burst (our CPU
    // interpreter runs far faster than the original 1.9968MHz chip did;
    // see lrescue_run_frame()'s doc comment), so total_cycles jumps in
    // steps rather than advancing smoothly in real time. It's still the
    // correct time axis for anything that needs to know the *relative*
    // real-time spacing between two port writes within or across frames --
    // e.g. lrescue_ports.cpp timestamps the speaker-bitstream port write
    // with this value, and lrescue_audio.cpp reconstructs that channel's
    // waveform by mapping its own audio-sample position onto this same
    // cycle axis via LRESCUE_CPU_HZ, rather than polling "what's the
    // level right now" -- which would have no way to represent a tune
    // that's supposed to unfold smoothly over real time.
    uint64_t total_cycles;
} arcade_system;

// Boot-error screen colors (RGB565), returned by lrescue_load_assets() so
// the sketch can drive hal_video with a solid-color diagnostic frame
// instead of starting the game. Same convention as ArcadeMachine_Invaders.
#define LRESCUE_COLOR_ERROR_NO_CARD   0xF800u // red    -- storage missing or won't mount
#define LRESCUE_COLOR_ERROR_NO_ASSETS 0xFFE0u // yellow -- mounted, but no ROM and/or sample files

// Sets game-state defaults and initializes video (calls hal_video_init()).
// Does not touch storage. The sketch must call hal_video_run() on whatever
// execution context drives display timing only *after* both this AND
// lrescue_load_assets() have returned -- see hal_video.h's hal_video_run()
// doc comment for why that ordering matters (identical constraint to
// ArcadeMachine_Invaders).
void lrescue_init(arcade_system *system);

// Loads ROM + PROM + audio assets via ArcadeHAL's storage contract, and
// finishes input-independent setup. On success returns true. On failure
// returns false and sets *out_error_color to one of the
// LRESCUE_COLOR_ERROR_* constants above.
bool lrescue_load_assets(arcade_system *system, uint16_t *out_error_color);

// Runs exactly one frame: executes CPU cycles for one video frame's worth
// of work (including the two mid-frame/vblank interrupts), then renders the
// frame via ArcadeHAL's video contract. Call this in a tight loop from the
// sketch after lrescue_input_update() has updated `system` for the frame.
void lrescue_run_frame(arcade_system *system);

#ifdef __cplusplus
}
#endif

#endif
