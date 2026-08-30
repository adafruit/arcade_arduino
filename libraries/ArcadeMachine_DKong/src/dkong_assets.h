// Donkey Kong ROM/PROM asset manifest -- board-agnostic (drives ArcadeHAL's
// storage contract). An explicit filename->destination manifest, NOT
// ArcadeMachine_Invaders' "sort filenames reverse-alphabetically, place
// consecutively" convention -- per DEVNOTES.md problem #12, that only works
// when a ROM set's chips map to one contiguous region. This set's 15 files
// split five ways: 4 program ROMs into the Z80's address space, 2 sound
// ROMs (loaded but unused -- see below), 2 tile ROMs and 4 sprite ROMs into
// decode-only staging buffers, and 3 PROMs into two more.
//
// The two sound ROMs are deliberately NOT loaded: this port has no sound
// hardware yet (see dkong_audio.h), so loading them would consume 4KB of
// SRAM to hold bytes nothing reads. They are listed in the sketch README as
// required on the SD card anyway, so that adding the 8035 later needs no
// change to anyone's card.
#ifndef DKONG_ASSETS_H
#define DKONG_ASSETS_H

#include <stdbool.h>
#include "dkong_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DKONG_ROM_LOAD_OK = 0,
    DKONG_ROM_LOAD_NO_STORAGE,   // storage missing or won't mount
    DKONG_ROM_LOAD_NO_ROM_FILES, // mounted, but /rom/ was missing required files
} dkong_rom_load_status_t;

// Mounts storage and loads every file in the manifest (see dkong_assets.cpp)
// from /rom/ into its real destination. Leaves storage mounted on success --
// the caller unmounts once dkong_video_build_caches() has consumed the
// staging buffers.
dkong_rom_load_status_t dkong_load_rom(dkong_system *system);

// Comma-separated list of manifest files that could not be loaded during
// the last dkong_load_rom() call ("" if none), each tagged "(short)" if it
// opened but read fewer bytes than expected. Valid until the next call.
// Exists so a boot failure can name the file instead of leaving the SD
// card's contents to guesswork.
const char *dkong_debug_missing_files(void);

#ifdef __cplusplus
}
#endif

#endif
