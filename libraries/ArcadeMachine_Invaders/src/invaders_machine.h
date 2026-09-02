// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// ArcadeMachine_Invaders: top-level Space Invaders machine state + lifecycle.
// Ported from invaders_pico's arcade.h/arcade.c. Board-agnostic: talks only
// to ArcadeHAL and ArcadeCPU_i8080, never to a specific board's libraries.
#ifndef INVADERS_MACHINE_H
#define INVADERS_MACHINE_H

#include <stdint.h>
#include <stdbool.h>
#include "i8080.h"

#ifdef __cplusplus
extern "C" {
#endif

// Native Space Invaders video RAM resolution (not the physical display's
// resolution -- see invaders_video.h for how this maps onto HAL_VIDEO_*).
#define INVADERS_GAME_WIDTH  256
#define INVADERS_GAME_HEIGHT 224

typedef struct {
    Cpu_state state; // CPU state (registers, sp, pc, memory, etc.)

    int left;
    int right;
    int shot;
    int start1;
    int start2;
    int coin;
    int tilt;

    uint16_t ext_shift_data;   // External shift register (shift data)
    uint8_t  ext_shift_offset; // External shift register (shift amount)
    uint8_t  dip_switches[8];  // Original SI hardware DIP switches
    uint8_t  arcade_mode[7];   // Color, Rotate, Flip, Fullscreen, Background, 2P_Vertical_Flip, Scaling_Mode

    uint8_t cocktail_vertical_screen_flip;
    bool    mirror_x;  // horizontal mirror toggle (e.g. for a Pepper's Ghost cabinet)
    uint8_t rotation;  // 0=landscape  1=90 deg CCW  2=180 deg  3=90 deg CW
} arcade_system;

// Boot-error screen colors (RGB565), returned by invaders_load_assets() so
// the sketch can drive hal_video with a solid-color diagnostic frame
// instead of starting the game.
#define INVADERS_COLOR_ERROR_NO_CARD   0xF800u // red    -- storage missing or won't mount
#define INVADERS_COLOR_ERROR_NO_ASSETS 0xFFE0u // yellow -- mounted, but no ROM and/or sample files

// Sets game-state defaults and initializes video (calls hal_video_init()).
// Does not touch storage. The sketch must call hal_video_run() on whatever
// execution context drives display timing only *after* both this AND
// invaders_load_assets() have returned -- see hal_video.h's hal_video_run()
// doc comment for why that ordering matters.
void invaders_init(arcade_system *system);

// Loads ROM + audio assets via ArcadeHAL's storage contract, and finishes
// input-independent setup. On success returns true. On failure returns
// false and sets *out_error_color to one of the INVADERS_COLOR_ERROR_*
// constants above, for the sketch to feed to a boot-error display loop.
bool invaders_load_assets(arcade_system *system, uint16_t *out_error_color);

// Runs exactly one frame: executes CPU cycles for one video frame's worth
// of work (including the two mid-frame/vblank interrupts the game expects),
// then renders the frame via ArcadeHAL's video contract. Call this in a
// tight loop from the sketch after invaders_input_update() has updated
// `system` for the frame.
void invaders_run_frame(arcade_system *system);

#ifdef __cplusplus
}
#endif

#endif
