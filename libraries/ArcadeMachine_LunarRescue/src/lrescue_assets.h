// Lunar Rescue ROM/PROM asset manifest -- board-agnostic (drives ArcadeHAL's
// storage contract).
//
// Unlike ArcadeMachine_Invaders' loader, this one canNOT use a
// "reverse-alphabetical filename sort -> consecutive addresses from 0x0000"
// convention: Lunar Rescue's own MAME ROM_START (midw8080/8080bw.cpp) loads
//   lrescue.1 -> 0x0000  lrescue.2 -> 0x0800  lrescue.3 -> 0x1000
//   lrescue.4 -> 0x1800  lrescue.5 -> 0x4000  lrescue.6 -> 0x4800
// -- i.e. a NON-consecutive gap (0x2000-0x3fff is RAM in between), exactly
// the scenario invaders_pico's/ArcadeMachine_Invaders' own DEVNOTES already
// flagged as unsupported by that convention (see the Space Invaders Deluxe
// note there). This loader uses an explicit filename->address table instead.
#ifndef LRESCUE_ASSETS_H
#define LRESCUE_ASSETS_H

#include <stdbool.h>
#include "lrescue_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LRESCUE_ROM_LOAD_OK = 0,
    LRESCUE_ROM_LOAD_NO_STORAGE,   // storage missing or won't mount
    LRESCUE_ROM_LOAD_NO_ROM_FILES, // mounted, but /rom/ was missing required chips
} lrescue_rom_load_status_t;

// Mounts storage, loads the six lrescue.N ROM chips from /rom/ into their
// fixed (non-consecutive) addresses in system->state.memory, loads the
// color-map PROM from /prom/7643-1.cpu into system->color_prom (missing PROM
// is a silent visual-only degradation, not a load failure -- see
// lrescue_machine.h), and sets default DIP-switch-equivalent configuration.
// Leaves storage mounted on success -- lrescue_audio_load_samples() loads
// /samples/*.wav next, then the caller should unmount. Never halts on
// failure; returns a status the caller uses to pick a boot-error color.
lrescue_rom_load_status_t lrescue_load_rom(arcade_system *system);

#ifdef __cplusplus
}
#endif

#endif
