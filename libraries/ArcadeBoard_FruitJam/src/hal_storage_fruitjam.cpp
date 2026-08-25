// hal_storage.h implementation for Adafruit Fruit Jam (SD card via the
// vendored FatFs + wili8jam SPI SD driver -- see fatfs/ and sdcard.c/.h in
// this library, ported verbatim from invaders_pico).
//
// Ported from invaders_pico's sd_loader.c, generalized from
// "ROM chips + numbered WAV files" (now Machine-layer knowledge in
// ArcadeMachine_Invaders's invaders_assets.cpp/invaders_audio.cpp) down to
// the plain mount/list/open/read/close primitive ArcadeHAL expects.
#include <string.h>
#include "fatfs/ff.h" // Arduino only adds this library's top-level src/ to
                       // the include path, not src/fatfs/ -- relative path
                       // needed to reach it from here.
#include "arcade_hal_storage.h"

static FATFS s_fs;
static bool  s_mounted = false;

bool hal_storage_mount(void) {
    if (s_mounted) return true;
    FRESULT r = f_mount(&s_fs, "", 1);
    s_mounted = (r == FR_OK);
    return s_mounted;
}

void hal_storage_unmount(void) {
    if (!s_mounted) return;
    f_unmount("");
    s_mounted = false;
}

bool hal_storage_list_dir(const char *dir, hal_storage_dirent_cb cb, void *ctx) {
    if (!s_mounted) return false;
    DIR d;
    if (f_opendir(&d, dir) != FR_OK) return false;

    FILINFO fno;
    for (;;) {
        if (f_readdir(&d, &fno) != FR_OK || fno.fname[0] == '\0') break;
        if (fno.fattrib & AM_DIR) continue;
        cb(fno.fname, ctx);
    }
    f_closedir(&d);
    return true;
}

// Sequential open/read/close only -- nothing in this codebase opens more
// than one file at a time (ROM chips and WAV samples are each loaded one
// file at a time). A tiny fixed pool keeps hal_file_t opaque without
// dynamic allocation.
struct hal_file {
    FIL  fil;
    bool in_use;
};

#define MAX_OPEN_FILES 2
static hal_file_t file_pool[MAX_OPEN_FILES];

hal_file_t *hal_storage_open(const char *path) {
    if (!s_mounted) return NULL;

    hal_file_t *slot = NULL;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!file_pool[i].in_use) { slot = &file_pool[i]; break; }
    }
    if (!slot) return NULL;

    if (f_open(&slot->fil, path, FA_READ) != FR_OK) return NULL;
    slot->in_use = true;
    return slot;
}

uint32_t hal_storage_read(hal_file_t *f, void *buf, uint32_t len) {
    if (!f) return 0;
    UINT br = 0;
    f_read(&f->fil, buf, (UINT)len, &br);
    return (uint32_t)br;
}

void hal_storage_close(hal_file_t *f) {
    if (!f) return;
    f_close(&f->fil);
    f->in_use = false;
}
