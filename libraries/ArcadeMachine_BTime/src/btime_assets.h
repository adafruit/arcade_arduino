// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Burger Time ROM asset manifest -- board-agnostic (drives ArcadeHAL's
// storage contract). An explicit filename->destination manifest, NOT
// ArcadeMachine_Invaders' "sort filenames reverse-alphabetically, place
// consecutively" convention: per DEVNOTES.md problem #12 that only works
// when a set's chips map to one contiguous region, and this set needs a
// manifest for two independent reasons.
//
//  1. THE PROGRAM ROMS ARE NOT IN FILENAME ORDER. ROM_START( btime ) loads
//     aa04.9b at 0xC000, aa06.13b at 0xD000, aa05.10b at 0xE000 and
//     aa07.15b at 0xF000 -- 04, 06, 05, 07. Sorting by name puts aa05
//     before aa06 and produces a machine that will not run.
//  2. `ab03.6b` IS NOT A GRAPHICS ROM. It is the background TILEMAP (which
//     16x16 tile goes in which cell), MAME's "bg_map" region, and it sits
//     in the middle of a run of files that otherwise are graphics.
#ifndef BTIME_ASSETS_H
#define BTIME_ASSETS_H

#include <stdbool.h>
#include "btime_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BTIME_ROM_LOAD_OK = 0,
    BTIME_ROM_LOAD_NO_STORAGE,   // storage missing or won't mount
    BTIME_ROM_LOAD_NO_ROM_FILES, // mounted, but /rom/ lacked required files
} btime_rom_load_status_t;

// Mounts storage and loads every file in the manifest (see btime_assets.cpp)
// from /rom/ into its real destination. Leaves storage mounted on success --
// the caller unmounts once video's tables are built.
btime_rom_load_status_t btime_load_rom(btime_system *system);

// Comma-separated list of manifest files that could not be loaded during
// the last btime_load_rom() call ("" if none), each tagged "(short)" if it
// opened but read fewer bytes than expected. Valid until the next call.
// Exists so a boot failure can name the file instead of leaving the SD
// card's contents to guesswork (DEVNOTES.md problem #43).
const char *btime_debug_missing_files(void);

#ifdef __cplusplus
}
#endif

#endif
