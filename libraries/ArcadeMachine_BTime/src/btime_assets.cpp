// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Burger Time ROM loading. Every filename, destination and size below is
// taken directly from MAME's ROM_START( btime )
// (src/mame/dataeast/btime.cpp:2546), and all 15 files' CRC32s were checked
// against that block before any of this was written -- the set in
// btime_assets/rom/ is the complete `btime` parent set.
#include <stdio.h>
#include <string.h>
#include "btime_assets.h"
#include "btime_video.h"
#include "arcade_hal_storage.h"

typedef struct {
    const char *filename;
    uint8_t    *dest;
    uint32_t    size;
} rom_file_t;

// Records every file the manifest could not load, so a boot failure can say
// WHICH file rather than just "required ROM files missing". Naming the file
// is the difference between a one-line fix and a guessing game about SD card
// contents; the generic message cost a real debugging cycle on Donkey Kong
// (DEVNOTES.md #43), and every new sketch should start with this rather than
// acquire it afterwards.
static char g_missing[128];
static unsigned g_missing_len;

const char *btime_debug_missing_files(void) {
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
    unsigned loaded = 0;
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
    return loaded == count;
}

btime_rom_load_status_t btime_load_rom(btime_system *system) {
    g_missing[0] = '\0';
    g_missing_len = 0;

    if (!hal_storage_mount()) return BTIME_ROM_LOAD_NO_STORAGE;

    // Main CPU program ROM. btime_map() maps 0xB000-0xFFFF as ROM but this
    // set only populates 0xC000-0xFFFF, so system->rom's first 0x1000 bytes
    // stay zeroed; see btime_ports.cpp's note on that hole.
    //
    // THE ORDER HERE IS THE ROM_START ORDER, NOT ALPHABETICAL: 04, 06, 05,
    // 07. This is the single easiest thing to get wrong about this set.
    uint8_t *const rom_c000 = system->rom + (0xC000 - BTIME_ROM_BASE);
    const rom_file_t program_rom[] = {
        { "aa04.9b",  rom_c000 + 0x0000, 0x1000 }, // 0xC000
        { "aa06.13b", rom_c000 + 0x1000, 0x1000 }, // 0xD000
        { "aa05.10b", rom_c000 + 0x2000, 0x1000 }, // 0xE000
        { "aa07.15b", rom_c000 + 0x3000, 0x1000 }, // 0xF000
    };
    if (!load_manifest(program_rom, 4)) {
        hal_storage_unmount();
        return BTIME_ROM_LOAD_NO_ROM_FILES;
    }

    // Sound CPU program ROM: one 4K chip at 0xE000, which audio_map()
    // mirrors at 0xF000 with .mirror(0x1000) -- so the 6502's reset and
    // interrupt vectors at 0xFFFA-0xFFFF are this chip's own top bytes.
    // The mirror is applied in btime_ports.cpp's decode (addr & 0x0FFF)
    // rather than by loading the chip twice.
    //
    // Treated as REQUIRED: without it the sound CPU resets to a vector read
    // out of zeroed memory and runs away through empty address space. It
    // does no harm (nothing it can reach affects the main CPU) but it burns
    // a third of the frame's emulation budget doing nothing, and a silent
    // machine is a better failure than a mysteriously slow one.
    const rom_file_t audio_rom[] = {
        { "ab14.12h", system->audio_rom, 0x1000 },
    };
    if (!load_manifest(audio_rom, 1)) {
        hal_storage_unmount();
        return BTIME_ROM_LOAD_NO_ROM_FILES;
    }

    // Graphics. A missing or short graphics ROM leaves that part of the
    // staging buffer zeroed and decodes to blank characters, sprites or
    // background -- a real but silent visual degradation rather than a boot
    // error, the same precedent ArcadeMachine_Pacman and _DKong set.
    //
    // gfx1 is BOTH the character set and the sprites: 0x6000 bytes = three
    // 0x2000 bitplanes, read as 1024 8x8 characters by one layout and as
    // 256 16x16 sprites by another. Same bytes, two interpretations.
    memset(btime_gfx1, 0, BTIME_GFX1_SIZE);
    memset(btime_gfx2, 0, BTIME_GFX2_SIZE);
    memset(btime_bg_map, 0, BTIME_BG_MAP_SIZE);

    const rom_file_t gfx[] = {
        { "aa12.7k",  btime_gfx1 + 0x0000, 0x1000 }, // charset #1 / sprites
        { "ab13.9k",  btime_gfx1 + 0x1000, 0x1000 },
        { "ab10.10k", btime_gfx1 + 0x2000, 0x1000 },
        { "ab11.12k", btime_gfx1 + 0x3000, 0x1000 },
        { "aa8.13k",  btime_gfx1 + 0x4000, 0x1000 },
        { "ab9.15k",  btime_gfx1 + 0x5000, 0x1000 },
        { "ab00.1b",  btime_gfx2 + 0x0000, 0x0800 }, // charset #2 (bg tiles)
        { "ab01.3b",  btime_gfx2 + 0x0800, 0x0800 },
        { "ab02.4b",  btime_gfx2 + 0x1000, 0x0800 },
        { "ab03.6b",  btime_bg_map,        0x0800 }, // background TILEMAP
    };
    load_manifest(gfx, 10);

    // Leave storage mounted -- btime_load_assets() unmounts once video's
    // tables are built.

    // DIP switch defaults, from INPUT_PORTS_START( btime ) (btime.cpp:1263).
    // Each value is the DIPSETTING that PORT_DIPNAME lists as its default
    // (its second argument), and note that these switches are ACTIVE LOW,
    // so "off" means the bit READS 1 -- which is why the defaults byte is
    // mostly ones rather than mostly zeros.
    //
    // DSW1 (location 15D on the sound PCB):
    //   0x03 Coin A          default 0x03 -> 1 coin / 1 credit
    //   0x0C Coin B          default 0x0C -> 1 coin / 1 credit
    //   0x10 "Leave Off"     default 0x10 -> off.  MAME: "Must be OFF. No
    //        test mode in ROM so this locks up the game at boot-up if on."
    //        THIS BIT MUST READ 1.
    //   0x20 unused          default 0x20 (PORT_DIPUNUSED_DIPLOC's default
    //        here is IP_ACTIVE_LOW, which is 0xffffffff in ioport.h, so the
    //        bit reads 1 -- NOT 0. DEVNOTES.md #24 is the record of an
    //        "unused" input bit being load-bearing.)
    //   0x40 Cabinet         default 0x00 -> Upright
    //   0x80 is NOT a DIP -- it is the vblank line, supplied per-scanline in
    //        btime_ports.cpp, and is masked out of this constant.
    system->dsw1 = 0x03 | 0x0C | 0x10 | 0x20; // = 0x3F, cabinet bit clear

    // DSW2 (location 14D):
    //   0x01 Lives           default 0x01 -> 3
    //   0x06 Bonus life      default 0x02 -> 20000
    //   0x08 Enemies         default 0x08 -> 4
    //   0x10 End-of-level pepper  default 0x00 -> Yes
    //   0x20/0x40/0x80 unused, all default set -> all read 1
    system->dsw2 = 0x01 | 0x02 | 0x08 | 0x00 | 0x20 | 0x40 | 0x80; // = 0xEB

    return BTIME_ROM_LOAD_OK;
}
