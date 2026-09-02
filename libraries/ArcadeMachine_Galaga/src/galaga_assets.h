// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Galaga ROM/PROM loading -- every filename/destination/size below is
// taken from MAME's ROM_START(galaga) (src/mame/namco/galaga.cpp),
// SHA1-verified byte-for-byte against galaga_assets/rom/'s actual files
// this session (not just filename-matched) -- see galaga_machine.h's
// header comment for the full citation trail and project memory
// (galaga-port-research.md) for the SHA1 comparison itself.
#ifndef GALAGA_ASSETS_H
#define GALAGA_ASSETS_H

#include <stdbool.h>
#include "galaga_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GALAGA_ROM_LOAD_OK,
    GALAGA_ROM_LOAD_NO_STORAGE,   // SD card missing or won't mount
    GALAGA_ROM_LOAD_NO_ROM_FILES  // mounted, but program ROM (any of the 3 CPUs') missing
} galaga_rom_load_status_t;

// Loads program ROM (all 3 CPUs, fatal if any chip is missing -- each
// CPU's program is a single contiguous unit, same "not independently
// useful" reasoning pacman_assets.cpp applies) plus gfx ROM/PROMs
// (non-fatal if missing -- degrades to garbled tiles/sprites/colors, same
// precedent pacman_assets.cpp/lrescue_assets.cpp set for their own gfx
// assets). Leaves storage mounted -- galaga_load_assets() unmounts once
// galaga_video_build_caches() has consumed the staging buffers.
galaga_rom_load_status_t galaga_load_rom(galaga_system *system);

#ifdef __cplusplus
}
#endif

#endif
