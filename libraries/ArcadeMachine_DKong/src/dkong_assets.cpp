// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Donkey Kong ROM/PROM loading. Every filename/destination/size below is
// taken directly from MAME's ROM_START( dkong ) (src/mame/nintendo/dkong.cpp).
#include <stdio.h>
#include <string.h>
#include "dkong_assets.h"
#include "dkong_video.h"
#include "dkong_audio.h"
#include "arcade_hal_storage.h"

// Reset per load so a retry does not accumulate stale names.


typedef struct {
    const char *filename;
    uint8_t    *dest;
    uint32_t    size;
} rom_file_t;

// Records every file the manifest could not load, so a boot failure can
// say WHICH file rather than just "required ROM files missing". Naming the
// file is the difference between a one-line fix and a guessing game about
// SD card contents; the generic message cost a real debugging cycle here.
static char g_missing[128];
static unsigned g_missing_len;

const char *dkong_debug_missing_files(void) {
    return g_missing_len ? g_missing : "";
}

static void note_missing(const char *name, bool short_read) {
    // Leave room for ", ..." plus the terminator.
    if (g_missing_len + 24 >= sizeof(g_missing)) return;
    if (g_missing_len) {
        g_missing[g_missing_len++] = ',';
        g_missing[g_missing_len++] = ' ';
    }
    for (const char *c = name; *c && g_missing_len < sizeof(g_missing) - 8; c++)
        g_missing[g_missing_len++] = *c;
    if (short_read) {
        const char *tag = "(short)";
        for (const char *c = tag; *c && g_missing_len < sizeof(g_missing) - 1; c++)
            g_missing[g_missing_len++] = *c;
    }
    g_missing[g_missing_len] = '\0';
}

static bool load_manifest(const rom_file_t *files, unsigned count) {
    int loaded = 0;
    for (unsigned i = 0; i < count; i++) {
        char path[40];
        snprintf(path, sizeof(path), "/rom/%s", files[i].filename);

        hal_file_t *f = hal_storage_open(path);
        if (!f) { note_missing(files[i].filename, false); continue; }

        uint32_t br = hal_storage_read(f, files[i].dest, files[i].size);
        hal_storage_close(f);
        if (br == files[i].size) loaded++;
        else note_missing(files[i].filename, true);
    }
    return loaded == (int)count;
}

dkong_rom_load_status_t dkong_load_rom(dkong_system *system) {
    g_missing[0] = '\0';
    g_missing_len = 0;

    if (!hal_storage_mount()) {
        return DKONG_ROM_LOAD_NO_STORAGE;
    }

    // Program ROM -- four 4K chips at 0x0000-0x3FFF. dkong_map() maps ROM
    // through 0x4FFF, but the `dkong` set has no chip for 0x4000-0x4FFF;
    // that window stays zeroed and is never executed.
    const rom_file_t program_rom[] = {
        { "c_5et_g.bin", system->rom + 0x0000, 0x1000 },
        { "c_5ct_g.bin", system->rom + 0x1000, 0x1000 },
        { "c_5bt_g.bin", system->rom + 0x2000, 0x1000 },
        { "c_5at_g.bin", system->rom + 0x3000, 0x1000 },
    };
    if (!load_manifest(program_rom, 4)) {
        hal_storage_unmount();
        return DKONG_ROM_LOAD_NO_ROM_FILES;
    }

    // Graphics. A missing/short gfx ROM leaves that part of the staging
    // buffer zeroed and decodes to blank tiles or sprites -- a real but
    // silent visual degradation rather than a boot-error condition, same
    // precedent as ArcadeMachine_Pacman's.
    memset(dkong_gfx1, 0, DKONG_GFX1_SIZE);
    memset(dkong_gfx2, 0, DKONG_GFX2_SIZE);
    const rom_file_t gfx[] = {
        { "v_5h_b.bin", dkong_gfx1 + 0x0000, 0x0800 },
        { "v_3pt.bin",  dkong_gfx1 + 0x0800, 0x0800 },
        { "l_4m_b.bin", dkong_gfx2 + 0x0000, 0x0800 },
        { "l_4n_b.bin", dkong_gfx2 + 0x0800, 0x0800 },
        { "l_4r_b.bin", dkong_gfx2 + 0x1000, 0x0800 },
        { "l_4s_b.bin", dkong_gfx2 + 0x1800, 0x0800 },
    };
    load_manifest(gfx, 6);

    // PROMs. ROM_START( dkong ) loads them into one 0x300-byte "proms"
    // region in this order: c-2k.bpr (palette low 4 bits) at 0x000,
    // c-2j.bpr (palette high 4 bits) at 0x100, v-5e.bpr (per-column
    // character colour codes) at 0x200. dkong2b_palette() then reads the
    // first two as one 512-byte block and takes the third from `+512`,
    // which is why the two palette PROMs share one buffer here and the
    // colour-code PROM has its own.
    memset(dkong_palette_prom, 0, DKONG_PALETTE_PROM_SIZE);
    memset(dkong_color_prom, 0, DKONG_COLOR_PROM_SIZE);
    const rom_file_t proms[] = {
        { "c-2k.bpr", dkong_palette_prom + 0x000, 0x100 },
        { "c-2j.bpr", dkong_palette_prom + 0x100, 0x100 },
        { "v-5e.bpr", dkong_color_prom,           0x100 },
    };
    load_manifest(proms, 3);

    // Sound ROMs. ROM_START( dkong ) loads s_3i_b.bin at 0x0000 and then
    // ROM_RELOADs the same 2K at 0x0800, so the 8035's 4K program space is
    // the one chip mirrored twice -- reproduced here rather than relying on
    // address masking, because the mirror is a fact about the board and the
    // masking would be an assumption about the code. s_3j_b.bin is the
    // sample ROM the 8035 reads in banked 256-byte pages.
    memset(dkong_sound_rom, 0, DKONG_SOUND_ROM_SIZE);
    memset(dkong_tune_rom, 0, DKONG_TUNE_ROM_SIZE);
    const rom_file_t sound_rom[] = {
        { "s_3i_b.bin", dkong_sound_rom + 0x0000, 0x0800 },
        { "s_3i_b.bin", dkong_sound_rom + 0x0800, 0x0800 }, // ROM_RELOAD
        { "s_3j_b.bin", dkong_tune_rom,           0x0800 },
    };
    // Not fatal: a set without these boots and plays silently, which is a
    // better outcome than refusing to run, and matches how the graphics
    // ROMs are treated above.
    load_manifest(sound_rom, 3);

    // Leave storage mounted -- dkong_load_assets() unmounts once
    // dkong_video_build_caches() has consumed the staging buffers above.

    // DSW0 default, from INPUT_PORTS_START( dkong_dsw0 ). Each value is the
    // DIPSETTING that dipname lists as its MAME default (the second
    // argument to PORT_DIPNAME):
    //   0x03 lives      default 0x00 -> 3 lives
    //   0x0c bonus life default 0x00 -> 7000 points
    //   0x70 coinage    default 0x00 -> 1 coin / 1 credit
    //   0x80 cabinet    default 0x80 -> Upright
    // Note this bank reads ACTIVE HIGH like the rest of this board's
    // inputs, so unlike Pac-Man's DSW1 the "all defaults" byte is mostly
    // zero rather than mostly one.
    system->dsw0 = 0x00 /* lives: 3 */
                 | 0x00 /* bonus life: 7000 */
                 | 0x00 /* coinage: 1C_1C */
                 | 0x80 /* cabinet: Upright */;

    return DKONG_ROM_LOAD_OK;
}
