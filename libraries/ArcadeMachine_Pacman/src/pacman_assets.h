// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Pac-Man ROM/PROM asset manifest -- board-agnostic (drives ArcadeHAL's
// storage contract). An explicit filename->destination manifest, NOT
// ArcadeMachine_Invaders' "sort filenames reverse-alphabetically, place
// consecutively" convention -- per arcade_arduino/DEVNOTES.md problem #12,
// that only works when a ROM set's chips map to one contiguous region.
// Pac-Man's real 10-file set splits three ways: 4 program ROMs into the
// Z80's address space, 2 graphics ROMs into a decode-only staging buffer
// (not CPU-addressable memory on real hardware), and 3 of its 4 PROMs into
// two more decode-only staging buffers (the 4th, 82s126.3m, is a "Timing"
// PROM MAME's own driver comments as "not used" -- not loaded here).
#ifndef PACMAN_ASSETS_H
#define PACMAN_ASSETS_H

#include <stdbool.h>
#include "pacman_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PACMAN_ROM_LOAD_OK = 0,
    PACMAN_ROM_LOAD_NO_STORAGE,   // storage missing or won't mount
    PACMAN_ROM_LOAD_NO_ROM_FILES, // mounted, but /rom/ was missing required files
} pacman_rom_load_status_t;

// Mounts storage and loads every file in the manifest (see pacman_assets.cpp)
// from /rom/ into its real destination (system->rom, or one of
// pacman_video.h's/pacman_audio.h's raw ROM staging buffers), and sets
// default DIP switch state. Leaves storage mounted on success -- the
// caller unmounts once pacman_video_build_caches() has consumed the
// staging buffers. Never halts on failure; returns a status the caller
// uses to pick a boot-error color (see pacman_machine.h's
// PACMAN_COLOR_ERROR_* constants).
pacman_rom_load_status_t pacman_load_rom(pacman_system *system);

#ifdef __cplusplus
}
#endif

#endif
