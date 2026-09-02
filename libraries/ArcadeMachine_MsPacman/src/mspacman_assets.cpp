// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Ms. Pac-Man ROM/PROM loading, plus the aux board's ROM decode.
//
// Every filename/destination/size below is taken directly from MAME's
// ROM_START( mspacman ) (src/mame/pacman/pacman.cpp), and the decode is a
// direct transcription of pacman_state::init_mspacman() and
// pacman_state::mspacman_install_patches() from the same file.
//
// Differences from ArcadeMachine_Pacman's manifest, all real:
//   - Three extra program ROMs: u5 (2K) at 0x8000, u6 (4K) at 0x9000 and
//     u7 (4K) at 0xB000 -- the daughterboard's own chips.
//   - The graphics ROMs are named "5e"/"5f", NOT "pacman.5e"/"pacman.5f".
//     Same 4K sizes and same destinations; only the dump filenames differ,
//     and a manifest that asks for the wrong names simply gets a silently
//     garbled tile/sprite set (see the gfx comment below for why that is
//     not a boot error).
//   - DSW1 defaults differ -- see the end of this file.
//   - 82s126.3m is still the unused "Timing" PROM and still not loaded.
#include <stdio.h>
#include <string.h>
#include "mspacman_assets.h"
#include "mspacman_video.h"
#include "mspacman_audio.h"
#include "arcade_hal_storage.h"

typedef struct {
    const char *filename;
    uint8_t    *dest;
    uint32_t    size;
} rom_file_t;

// Records every file the manifest could not load, so a boot failure can name
// the file rather than just a category. Added after a red screen on this
// game could not be told apart from a DVI starvation red without one --
// see DEVNOTES.md #43 and #49.
static char g_missing[128];
static unsigned g_missing_len;

const char *mspacman_debug_missing_files(void) {
    return g_missing_len ? g_missing : "";
}

static void note_missing(const char *name, bool short_read) {
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
        char path[32];
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

// MAME's bitswap<N>(val, b_{N-1}, ..., b_0) selects bits of `val` by
// position, most significant FIRST: the leftmost argument becomes the
// result's bit N-1. Transcribed here rather than pulled in as a generic
// helper so each call site below stays visually one-to-one with the MAME
// line it came from -- these argument orders are the entire decode, and a
// transposition in one of them is not something a compiler or a boot test
// will catch (the game simply runs wrong code).
static inline uint8_t bitswap8(uint8_t v, int b7, int b6, int b5, int b4,
                                          int b3, int b2, int b1, int b0) {
    return (uint8_t)(((v >> b7) & 1) << 7 | ((v >> b6) & 1) << 6 |
                     ((v >> b5) & 1) << 5 | ((v >> b4) & 1) << 4 |
                     ((v >> b3) & 1) << 3 | ((v >> b2) & 1) << 2 |
                     ((v >> b1) & 1) << 1 | ((v >> b0) & 1));
}

static inline uint16_t bitswap11(uint16_t v, int b10, int b9, int b8, int b7,
                                             int b6, int b5, int b4, int b3,
                                             int b2, int b1, int b0) {
    return (uint16_t)(((v >> b10) & 1) << 10 | ((v >> b9) & 1) << 9 |
                      ((v >> b8)  & 1) <<  8 | ((v >> b7) & 1) << 7 |
                      ((v >> b6)  & 1) <<  6 | ((v >> b5) & 1) << 5 |
                      ((v >> b4)  & 1) <<  4 | ((v >> b3) & 1) << 3 |
                      ((v >> b2)  & 1) <<  2 | ((v >> b1) & 1) << 1 |
                      ((v >> b0)  & 1));
}

static inline uint16_t bitswap12(uint16_t v, int b11, int b10, int b9, int b8,
                                             int b7, int b6, int b5, int b4,
                                             int b3, int b2, int b1, int b0) {
    return (uint16_t)(((v >> b11) & 1) << 11 | ((v >> b10) & 1) << 10 |
                      ((v >> b9)  & 1) <<  9 | ((v >> b8)  & 1) <<  8 |
                      ((v >> b7)  & 1) <<  7 | ((v >> b6)  & 1) <<  6 |
                      ((v >> b5)  & 1) <<  5 | ((v >> b4)  & 1) <<  4 |
                      ((v >> b3)  & 1) <<  3 | ((v >> b2)  & 1) <<  2 |
                      ((v >> b1)  & 1) <<  1 | ((v >> b0)  & 1));
}

// Forty 8-byte patches copied out of the decrypted u5 image (which lands at
// 0x8000-0x81FF of the decrypted bank) over the Pac-Man code beneath it.
// Transcribed verbatim from MAME's mspacman_install_patches(); `ROM` there
// is the decrypted bank, so both sides of every assignment index the SAME
// bank -- a detail worth stating because it is the natural thing to get
// wrong when splitting MAME's single 128K region into two banks.
static void install_patches(uint8_t *rom) {
    for (int i = 0; i < 8; i++) {
        rom[0x0410+i] = rom[0x8008+i];
        rom[0x08e0+i] = rom[0x81d8+i];
        rom[0x0a30+i] = rom[0x8118+i];
        rom[0x0bd0+i] = rom[0x80d8+i];
        rom[0x0c20+i] = rom[0x8120+i];
        rom[0x0e58+i] = rom[0x8168+i];
        rom[0x0ea8+i] = rom[0x8198+i];

        rom[0x1000+i] = rom[0x8020+i];
        rom[0x1008+i] = rom[0x8010+i];
        rom[0x1288+i] = rom[0x8098+i];
        rom[0x1348+i] = rom[0x8048+i];
        rom[0x1688+i] = rom[0x8088+i];
        rom[0x16b0+i] = rom[0x8188+i];
        rom[0x16d8+i] = rom[0x80c8+i];
        rom[0x16f8+i] = rom[0x81c8+i];
        rom[0x19a8+i] = rom[0x80a8+i];
        rom[0x19b8+i] = rom[0x81a8+i];

        rom[0x2060+i] = rom[0x8148+i];
        rom[0x2108+i] = rom[0x8018+i];
        rom[0x21a0+i] = rom[0x81a0+i];
        rom[0x2298+i] = rom[0x80a0+i];
        rom[0x23e0+i] = rom[0x80e8+i];
        rom[0x2418+i] = rom[0x8000+i];
        rom[0x2448+i] = rom[0x8058+i];
        rom[0x2470+i] = rom[0x8140+i];
        rom[0x2488+i] = rom[0x8080+i];
        rom[0x24b0+i] = rom[0x8180+i];
        rom[0x24d8+i] = rom[0x80c0+i];
        rom[0x24f8+i] = rom[0x81c0+i];
        rom[0x2748+i] = rom[0x8050+i];
        rom[0x2780+i] = rom[0x8090+i];
        rom[0x27b8+i] = rom[0x8190+i];
        rom[0x2800+i] = rom[0x8028+i];
        rom[0x2b20+i] = rom[0x8100+i];
        rom[0x2b30+i] = rom[0x8110+i];
        rom[0x2bf0+i] = rom[0x81d0+i];
        rom[0x2cc0+i] = rom[0x80d0+i];
        rom[0x2cd8+i] = rom[0x80e0+i];
        rom[0x2cf0+i] = rom[0x81e0+i];
        rom[0x2d60+i] = rom[0x8160+i];
    }
}

// Builds the decrypted bank from the plain one, then rewrites the plain
// bank's high window. Transcribed from MAME's init_mspacman().
//
// ORDER IS LOAD-BEARING and is the one thing to preserve if this is ever
// refactored: the decode reads the RAW u5/u6/u7 images out of the plain
// bank at 0x8000/0x9000/0xB000, and the last step then OVERWRITES that same
// region with mirrors of the Pac-Man ROMs. Doing those two steps in the
// other order silently decodes the wrong bytes -- the aux ROMs would be
// gone by the time they were read.
static void build_decrypted_bank(mspacman_system *system) {
    uint8_t *rom  = system->rom[MSPACMAN_BANK_PLAIN];
    uint8_t *drom = system->rom[MSPACMAN_BANK_DECRYPTED];

    for (int i = 0; i < 0x1000; i++) {
        drom[0x0000+i] = rom[0x0000+i]; // pacman.6e
        drom[0x1000+i] = rom[0x1000+i]; // pacman.6f
        drom[0x2000+i] = rom[0x2000+i]; // pacman.6h
        drom[0x3000+i] = bitswap8(rom[0xb000+bitswap12((uint16_t)i,11,3,7,9,10,8,6,5,4,2,1,0)],0,4,5,7,6,3,2,1); // decrypt u7
    }
    for (int i = 0; i < 0x800; i++) {
        drom[0x8000+i] = bitswap8(rom[0x8000+bitswap11((uint16_t)i,8,7,5,9,10,6,3,4,2,1,0)],0,4,5,7,6,3,2,1); // decrypt u5
        drom[0x8800+i] = bitswap8(rom[0x9800+bitswap11((uint16_t)i,3,7,9,10,8,6,5,4,2,1,0)],0,4,5,7,6,3,2,1); // decrypt half of u6
        drom[0x9000+i] = bitswap8(rom[0x9000+bitswap11((uint16_t)i,3,7,9,10,8,6,5,4,2,1,0)],0,4,5,7,6,3,2,1); // decrypt half of u6
        drom[0x9800+i] = rom[0x1800+i]; // mirror of pacman.6f high
    }
    for (int i = 0; i < 0x1000; i++) {
        drom[0xa000+i] = rom[0x2000+i]; // mirror of pacman.6h
        drom[0xb000+i] = rom[0x3000+i]; // mirror of pacman.6j
    }

    // install patches into decrypted bank
    install_patches(drom);

    // mirror Pac-Man ROMs into upper addresses of normal bank -- this is the
    // step that destroys the raw u5/u6/u7 images read above.
    for (int i = 0; i < 0x1000; i++) {
        rom[0x8000+i] = rom[0x0000+i];
        rom[0x9000+i] = rom[0x1000+i];
        rom[0xa000+i] = rom[0x2000+i];
        rom[0xb000+i] = rom[0x3000+i];
    }
}

mspacman_rom_load_status_t mspacman_load_rom(mspacman_system *system) {
    g_missing[0] = '\0';
    g_missing_len = 0;

    if (!hal_storage_mount()) {
        return MSPACMAN_ROM_LOAD_NO_STORAGE;
    }

    uint8_t *plain = system->rom[MSPACMAN_BANK_PLAIN];

    // Program ROM. The four Pac-Man chips load consecutively at
    // 0x0000-0x3FFF; the three aux-board chips land at the addresses
    // ROM_START( mspacman ) gives them, which are where build_decrypted_bank()
    // expects to read them from. u5 is 2K, u6 and u7 are 4K, and u6 loads at
    // 0x9000 while u7 loads at 0xB000 -- note the 0xA000 gap, which is real.
    const rom_file_t program_rom[] = {
        { "pacman.6e", plain + 0x0000, 0x1000 },
        { "pacman.6f", plain + 0x1000, 0x1000 },
        { "pacman.6h", plain + 0x2000, 0x1000 },
        { "pacman.6j", plain + 0x3000, 0x1000 },
        { "u5",        plain + 0x8000, 0x0800 },
        { "u6",        plain + 0x9000, 0x1000 },
        { "u7",        plain + 0xB000, 0x1000 },
    };
    // All seven are required. Unlike the graphics ROMs below, a missing one
    // here is fatal: without u5/u6/u7 the decode produces a bank full of
    // decrypted zeroes and the machine runs plain Pac-Man code with forty
    // 8-byte holes punched through it, which is far worse than a clean
    // boot-error screen.
    if (!load_manifest(program_rom, 7)) {
        hal_storage_unmount();
        return MSPACMAN_ROM_LOAD_NO_ROM_FILES;
    }

    build_decrypted_bank(system);

    // Graphics ROMs -- "5e" (tiles) then "5f" (sprites), concatenated into
    // one gfx1-shaped 0x2000 buffer (see mspacman_video.h's
    // MSPACMAN_GFX_ROM_SIZE and mspacman_video.cpp's decode loops, which index
    // tiles at offset 0x0000 and sprites at offset 0x1000, matching MAME's
    // own GFXDECODE_ENTRY offsets). A missing/short gfx ROM leaves that
    // half of mspacman_gfx_rom zeroed -- mspacman_video_build_caches() then
    // decodes garbage tiles/sprites (a real but silent visual degradation,
    // matching lrescue_assets.cpp's precedent for its color PROM), not a
    // boot-error condition, since gameplay logic doesn't depend on it.
    memset(mspacman_gfx_rom, 0, MSPACMAN_GFX_ROM_SIZE);
    const rom_file_t gfx_rom[] = {
        { "5e", mspacman_gfx_rom + 0x0000, 0x1000 },
        { "5f", mspacman_gfx_rom + 0x1000, 0x1000 },
    };
    load_manifest(gfx_rom, 2);

    // Color PROMs -- identical files to Pac-Man's, and MAME's own ROM_START
    // uses the same names and CRCs. 82s126.1m (the WSG waveform table) is
    // loaded by mspacman_audio.h's own staging buffer below. 82s126.3m
    // ("Timing - not used", per MAME's own ROM_START comment) is
    // deliberately never loaded.
    memset(mspacman_palette_prom, 0, MSPACMAN_PALETTE_PROM_SIZE);
    memset(mspacman_lookup_prom, 0, MSPACMAN_LOOKUP_PROM_SIZE);
    const rom_file_t color_proms[] = {
        { "82s123.7f", mspacman_palette_prom, MSPACMAN_PALETTE_PROM_SIZE },
        { "82s126.4a", mspacman_lookup_prom,  MSPACMAN_LOOKUP_PROM_SIZE },
    };
    load_manifest(color_proms, 2);

    // WSG waveform PROM -- see mspacman_audio.h.
    memset(mspacman_wave_prom, 0, MSPACMAN_WAVE_PROM_SIZE);
    const rom_file_t wave_prom[] = {
        { "82s126.1m", mspacman_wave_prom, MSPACMAN_WAVE_PROM_SIZE },
    };
    load_manifest(wave_prom, 1);

    // Leave storage mounted -- mspacman_load_assets() unmounts once
    // mspacman_video_build_caches() has consumed the staging buffers above.

    // DSW1 default, from INPUT_PORTS_START( mspacman ) -- a separate port
    // definition from pacman's, not a PORT_INCLUDE of it, and the defaults
    // genuinely differ. Each value below is the DIPSETTING that dipname
    // lists as its MAME default (the second argument to PORT_DIPNAME):
    //   0x03 coinage    default 0x01 -> 1 coin / 1 credit
    //   0x0c lives      default 0x08 -> 3 lives
    //   0x30 bonus life default 0x00 -> 10000 points
    //   0x40 difficulty default 0x40 -> Normal
    //   0x80            IPT_UNUSED (active low)
    // The one that differs from Pac-Man is bit 7: Pac-Man uses it for
    // "ghost names" (Normal/Alternate) and defaults it set, whereas here it
    // is PORT_BIT(0x80, IP_ACTIVE_LOW, IPT_UNUSED). Active-low unused reads
    // as 1, so the byte is the same either way -- but the reason is
    // different, and DEVNOTES.md problem #24 is a whole entry about assuming
    // an unused DIP bit reads as 0.
    system->dsw1 = 0x01 /* coinage: 1C_1C */
                 | 0x08 /* lives: 3 */
                 | 0x00 /* bonus life: 10000 */
                 | 0x40 /* difficulty: Normal */
                 | 0x80 /* unused, active low -> reads 1 */;

    return MSPACMAN_ROM_LOAD_OK;
}
