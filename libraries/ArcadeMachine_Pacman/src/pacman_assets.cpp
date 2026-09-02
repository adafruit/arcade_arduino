// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Pac-Man ROM/PROM loading. Every filename/destination/size below is taken
// directly from MAME's ROM_START( pacman ) (src/mame/pacman/pacman.cpp) --
// see pacman_assets.h for why this can't reuse ArcadeMachine_Invaders'
// sort-and-pack convention.
#include <stdio.h>
#include <string.h>
#include "pacman_assets.h"
#include "pacman_video.h"
#include "pacman_audio.h"
#include "arcade_hal_storage.h"

typedef struct {
    const char *filename;
    uint8_t    *dest;
    uint32_t    size;
} rom_file_t;

static bool load_manifest(const rom_file_t *files, unsigned count) {
    int loaded = 0;
    for (unsigned i = 0; i < count; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/rom/%s", files[i].filename);

        hal_file_t *f = hal_storage_open(path);
        if (!f) continue;

        uint32_t br = hal_storage_read(f, files[i].dest, files[i].size);
        hal_storage_close(f);
        if (br == files[i].size) loaded++;
    }
    return loaded == (int)count;
}

pacman_rom_load_status_t pacman_load_rom(pacman_system *system) {
    if (!hal_storage_mount()) {
        return PACMAN_ROM_LOAD_NO_STORAGE;
    }

    // Program ROM -- pacman.6e/6f/6h/6j load consecutively into the Z80's
    // 0x0000-0x3FFF address space.
    const rom_file_t program_rom[] = {
        { "pacman.6e", system->rom + 0x0000, 0x1000 },
        { "pacman.6f", system->rom + 0x1000, 0x1000 },
        { "pacman.6h", system->rom + 0x2000, 0x1000 },
        { "pacman.6j", system->rom + 0x3000, 0x1000 },
    };
    // Program ROM is not optional -- a set missing any chip can't run at
    // all (unlike Lunar Rescue's "at least the low 4 chips" best-effort
    // rule, Pac-Man's 4 chips are a single contiguous unit with no
    // independently-useful subset).
    if (!load_manifest(program_rom, 4)) {
        hal_storage_unmount();
        return PACMAN_ROM_LOAD_NO_ROM_FILES;
    }

    // Graphics ROMs -- pacman.5e (tiles) then pacman.5f (sprites),
    // concatenated into one gfx1-shaped 0x2000 buffer (see pacman_video.h's
    // PACMAN_GFX_ROM_SIZE and pacman_video.cpp's decode loops, which index
    // tiles at offset 0x0000 and sprites at offset 0x1000, matching MAME's
    // own GFXDECODE_ENTRY offsets). A missing/short gfx ROM leaves that
    // half of pacman_gfx_rom zeroed -- pacman_video_build_caches() then
    // decodes garbage tiles/sprites (a real but silent visual degradation,
    // matching lrescue_assets.cpp's precedent for its color PROM), not a
    // boot-error condition, since gameplay logic doesn't depend on it.
    memset(pacman_gfx_rom, 0, PACMAN_GFX_ROM_SIZE);
    const rom_file_t gfx_rom[] = {
        { "pacman.5e", pacman_gfx_rom + 0x0000, 0x1000 },
        { "pacman.5f", pacman_gfx_rom + 0x1000, 0x1000 },
    };
    load_manifest(gfx_rom, 2);

    // Color PROMs -- 82s123.7f (32-byte palette) and 82s126.4a (256-byte
    // lookup table). 82s126.1m (the WSG waveform table) is loaded by
    // pacman_audio.h's own staging buffer below, not here, since it feeds
    // sound rather than video. 82s126.3m ("Timing" PROM, per MAME's own
    // ROM_START comment: "Timing - not used") is deliberately never
    // loaded -- it exists on the real board's SD card ROM set but has no
    // effect on emulation.
    memset(pacman_palette_prom, 0, PACMAN_PALETTE_PROM_SIZE);
    memset(pacman_lookup_prom, 0, PACMAN_LOOKUP_PROM_SIZE);
    const rom_file_t color_proms[] = {
        { "82s123.7f", pacman_palette_prom, PACMAN_PALETTE_PROM_SIZE },
        { "82s126.4a", pacman_lookup_prom,  PACMAN_LOOKUP_PROM_SIZE },
    };
    load_manifest(color_proms, 2);

    // WSG waveform PROM -- see pacman_audio.h.
    memset(pacman_wave_prom, 0, PACMAN_WAVE_PROM_SIZE);
    const rom_file_t wave_prom[] = {
        { "82s126.1m", pacman_wave_prom, PACMAN_WAVE_PROM_SIZE },
    };
    load_manifest(wave_prom, 1);

    // Leave storage mounted -- pacman_load_assets() unmounts once
    // pacman_video_build_caches() has consumed the staging buffers above.

    // DSW1 default: 1 coin/1 credit, 3 lives, bonus life at 10000, Normal
    // difficulty, Normal ghost names -- bit layout verified against MAME's
    // INPUT_PORTS_START(pacman) (all these are the DIPSETTING each dipname
    // lists as its MAME default). DSW1 is active-low like IN0/IN1's
    // buttons, but each DIP *pair*/nibble here is a direct value (not a
    // single active-low bit) -- these are the raw byte values MAME's
    // PORT_DIPNAME defaults specify, not inverted.
    system->dsw1 = 0x01 /* coinage: 1C_1C */
                 | 0x08 /* lives: 3 */
                 | 0x00 /* bonus life: 10000 */
                 | 0x40 /* difficulty: Normal */
                 | 0x80 /* ghost names: Normal */;

    return PACMAN_ROM_LOAD_OK;
}
