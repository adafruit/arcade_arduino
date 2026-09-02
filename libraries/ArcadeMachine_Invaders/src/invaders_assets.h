// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Space Invaders ROM/asset manifest -- board-agnostic (drives ArcadeHAL's
// storage contract). Ported from invaders_pico's rom_loader.c and the ROM
// half of sd_loader.c.
#ifndef INVADERS_ASSETS_H
#define INVADERS_ASSETS_H

#include <stdbool.h>
#include "invaders_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    INVADERS_ROM_LOAD_OK = 0,
    INVADERS_ROM_LOAD_NO_STORAGE,   // storage missing or won't mount
    INVADERS_ROM_LOAD_NO_ROM_FILES, // mounted, but /rom/ had no usable ROM files
} invaders_rom_load_status_t;

// Mounts storage, loads ROM chip files from /rom/ into system->state.memory
// (see invaders_assets.cpp for the file-ordering convention), and sets
// default DIP switch / arcade mode configuration. Leaves storage mounted on
// success -- invaders_audio_load_samples() loads /samples/*.wav next, then
// the caller should unmount. Never halts on failure; returns a status the
// caller uses to pick a boot-error color (see invaders_machine.h's
// INVADERS_COLOR_ERROR_* constants).
invaders_rom_load_status_t invaders_load_rom(arcade_system *system);

#ifdef __cplusplus
}
#endif

#endif
