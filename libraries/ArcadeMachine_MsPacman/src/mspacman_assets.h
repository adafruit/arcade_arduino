// Ms. Pac-Man ROM/PROM asset manifest -- board-agnostic (drives ArcadeHAL's
// storage contract). An explicit filename->destination manifest, NOT
// ArcadeMachine_Invaders' "sort filenames reverse-alphabetically, place
// consecutively" convention -- per arcade_arduino/DEVNOTES.md problem #12,
// that only works when a ROM set's chips map to one contiguous region.
// This set is the clearest case in the project of why: its 13 files split
// four ways, and the program ROMs are not even contiguous with each other
// (u5 at 0x8000, u6 at 0x9000, u7 at 0xB000, with a deliberate gap at
// 0xA000). They are: 4 Pac-Man program ROMs plus 3 aux-board ROMs into the
// Z80's address space, 2 graphics ROMs into a decode-only staging buffer
// (not CPU-addressable memory on real hardware), and 3 of its 4 PROMs into
// two more decode-only staging buffers (the 4th, 82s126.3m, is a "Timing"
// PROM MAME's own driver comments as "not used" -- not loaded here).
//
// Loading is also where this machine's ROM DECODE happens: the aux board's
// u5/u6/u7 are address- and data-line scrambled, and the decrypted bank is
// built from them at load time. See mspacman_assets.cpp.
#ifndef MSPACMAN_ASSETS_H
#define MSPACMAN_ASSETS_H

#include <stdbool.h>
#include "mspacman_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MSPACMAN_ROM_LOAD_OK = 0,
    MSPACMAN_ROM_LOAD_NO_STORAGE,   // storage missing or won't mount
    MSPACMAN_ROM_LOAD_NO_ROM_FILES, // mounted, but /rom/ was missing required files
} mspacman_rom_load_status_t;

// Mounts storage and loads every file in the manifest (see mspacman_assets.cpp)
// from /rom/ into its real destination (system->rom, or one of
// mspacman_video.h's/mspacman_audio.h's raw ROM staging buffers), and sets
// default DIP switch state. Leaves storage mounted on success -- the
// caller unmounts once mspacman_video_build_caches() has consumed the
// staging buffers. Never halts on failure; returns a status the caller
// uses to pick a boot-error color (see mspacman_machine.h's
// MSPACMAN_COLOR_ERROR_* constants).
mspacman_rom_load_status_t mspacman_load_rom(mspacman_system *system);

// Comma-separated list of manifest files that could not be loaded during the
// last mspacman_load_rom() call ("" if none), each tagged "(short)" if it
// opened but read fewer bytes than expected.
const char *mspacman_debug_missing_files(void);

#ifdef __cplusplus
}
#endif

#endif
