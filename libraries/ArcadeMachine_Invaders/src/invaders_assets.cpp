// Space Invaders ROM loading -- ported from invaders_pico's rom_loader.c
// and the ROM half of sd_loader.c, rewired onto ArcadeHAL's storage
// contract instead of calling FatFs directly.
#include <stdio.h>
#include <string.h>
#include "invaders_assets.h"
#include "arcade_hal_storage.h"

#define MAX_ROM_FILES 8
#define MAX_FNAME     256

typedef struct {
    char names[MAX_ROM_FILES][MAX_FNAME];
    int  count;
} rom_filelist_t;

static void collect_rom_file(const char *filename, void *ctx) {
    rom_filelist_t *list = (rom_filelist_t *)ctx;
    // Skip dotfiles -- macOS Finder silently drops AppleDouble sidecar
    // files (._invaders.h) and .DS_Store onto any non-HFS+/APFS volume
    // (which a FAT32 SD card always is) whenever files are copied via
    // Finder. Without this, they'd compete with real ROM chip files for
    // MAX_ROM_FILES collection slots -- harmless if the total happens to
    // stay within the cap (as it did during initial hardware testing, by
    // reverse-alpha-sort coincidence: '.' sorts below 'i'), but a silent
    // ROM-corrupting landmine otherwise.
    if (filename[0] == '.') return;
    if (list->count >= MAX_ROM_FILES) return;
    strncpy(list->names[list->count], filename, MAX_FNAME - 1);
    list->names[list->count][MAX_FNAME - 1] = '\0';
    list->count++;
}

// Simple reverse-alphabetical sort (bubble sort -- tiny N). The standard
// MAME chip naming (.h -> .g -> .f -> .e) sorts into the correct address
// order this way; see invaders_pico's README.md for the convention.
static void sort_reverse_alpha(char names[][MAX_FNAME], int count) {
    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - i - 1; j++) {
            if (strcmp(names[j], names[j + 1]) < 0) {
                char tmp[MAX_FNAME];
                strcpy(tmp, names[j]);
                strcpy(names[j], names[j + 1]);
                strcpy(names[j + 1], tmp);
            }
        }
    }
}

static bool load_rom_files(uint8_t *memory) {
    rom_filelist_t list;
    list.count = 0;
    if (!hal_storage_list_dir("/rom", collect_rom_file, &list)) return false;
    if (list.count == 0) return false;

    sort_reverse_alpha(list.names, list.count);

    uint32_t addr = 0;
    for (int i = 0; i < list.count && addr < 0x2000; i++) {
        char path[MAX_FNAME + 8];
        snprintf(path, sizeof(path), "/rom/%s", list.names[i]);

        hal_file_t *f = hal_storage_open(path);
        if (!f) continue;

        uint32_t to_read = 0x2000u - addr;
        uint32_t br = hal_storage_read(f, memory + addr, to_read);
        hal_storage_close(f);
        addr += br;
    }

    return addr > 0;
}

invaders_rom_load_status_t invaders_load_rom(arcade_system *system) {
    if (!hal_storage_mount()) {
        return INVADERS_ROM_LOAD_NO_STORAGE;
    }
    if (!load_rom_files(system->state.memory)) {
        hal_storage_unmount();
        return INVADERS_ROM_LOAD_NO_ROM_FILES;
    }
    // Leave storage mounted -- invaders_audio_load_samples() loads WAV
    // files next, then the caller unmounts.

    // DIP switches matching invaders.ini defaults:
    // SW1=1 SW2=1 -> 3 ships; SW4=1 -> bonus at 1500pts; SW5-7 always 1; SW8=1
    system->dip_switches[0] = 1; // SW1
    system->dip_switches[1] = 1; // SW2
    system->dip_switches[2] = 0; // SW3 (RAM/sound check OFF)
    system->dip_switches[3] = 1; // SW4
    system->dip_switches[4] = 1; // SW5
    system->dip_switches[5] = 1; // SW6
    system->dip_switches[6] = 1; // SW7
    system->dip_switches[7] = 1; // SW8

    system->arcade_mode[0] = 1;
    system->arcade_mode[1] = 1;
    system->arcade_mode[2] = 0;
    system->arcade_mode[3] = 0;
    system->arcade_mode[4] = 0;
    system->arcade_mode[5] = 0;
    system->arcade_mode[6] = 0;

    return INVADERS_ROM_LOAD_OK;
}
