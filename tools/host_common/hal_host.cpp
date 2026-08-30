// Host-harness ArcadeHAL backend, shared by every *_host harness under
// arcade_arduino/tools/ -- see ../README.md.
//
// This is a fourth "board" implementing the same ArcadeHAL contract that
// ArcadeBoard_FruitJam does (13 functions total), but backed by stdio and
// plain memory instead of DVI/GPIO/SD. Because every ArcadeMachine_* file
// is board-agnostic by SAMP's own design rule -- machine code talks ONLY
// through ArcadeHAL, never to a board library -- the entire Galaga machine
// (CPU cores, port decode, video, asset loading) compiles and runs
// unmodified on a Mac against this file.
//
// Video geometry is copied from ArcadeBoard_FruitJam's real values
// (hal_video_fruitjam.cpp: 640x480, DVI_VERTICAL_REPEAT_USED 2 ->
// 240 scanlines/frame) so the machine layer's per-frame CPU/scanline
// interleaving executes with the SAME shape it does on hardware. Changing
// these would change how run_frame_interleaved() slices CPU time, which is
// exactly the behaviour under investigation -- keep them in sync with the
// board if the board's ever change.
#include "arcade_hal_video.h"
#include "arcade_hal_input.h"
#include "arcade_hal_storage.h"
#include "arcade_hal_audio.h"
#include <Arduino.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

HostSerial Serial;

// --- video ---------------------------------------------------------------

const uint32_t HAL_VIDEO_WIDTH               = 640u;
const uint32_t HAL_VIDEO_HEIGHT              = 480u;
const uint32_t HAL_VIDEO_SCANLINES_PER_FRAME = 240u; // 480 / vertical repeat 2

static uint16_t host_scanbuf[640];

bool hal_video_init(void) { return true; }

// Single shared buffer: the machine layer fills it and immediately
// "submits" it, and nothing here reads pixels back, so there's no need to
// model the board's real multi-buffer scanline queue. (If a future test
// wants to assert on rendered output, capture inside submit instead.)
uint16_t *hal_video_acquire_scanline(void) { return host_scanbuf; }
void hal_video_submit_scanline(uint16_t *buf) { (void)buf; }
void hal_video_run(void) { for (;;) { } } // never called by the harness

// Nothing blocks on the host, so no time is ever spent waiting for the
// display backend (see arcade_hal_video.h).
uint32_t hal_video_take_blocked_us(void) { return 0; }

// --- input ---------------------------------------------------------------

const uint8_t HAL_INPUT_BUTTON_COUNT = 7; // matches ArcadeBoard_FruitJam's pin table

void hal_input_init(void) {}

// All buttons released. NOTE the harness still calls galaga_input_update()
// every frame rather than leaving in0/in1 at their memset-zero defaults:
// Galaga's inputs are ACTIVE LOW, so all-zero shadow bytes would read as
// every button held down (coin permanently inserted, etc).
bool hal_input_read(uint8_t index) { (void)index; return false; }

// --- audio ---------------------------------------------------------------

// Silent sink. The machine layer still runs its full synthesis path (the
// fill callback is registered and can be driven by a harness that wants to
// assert on generated samples); nothing is played. Kept minimal on purpose
// -- audio correctness is not what these harnesses are for.
static hal_audio_fill_cb g_audio_cb = NULL;

bool hal_audio_init(uint32_t sample_rate) { (void)sample_rate; return true; }
void hal_audio_set_fill_callback(hal_audio_fill_cb cb) { g_audio_cb = cb; }

// No ISR on the host, so there is no critical section to enter.
uint32_t hal_audio_enter_critical(void) { return 0; }
void hal_audio_exit_critical(uint32_t saved_state) { (void)saved_state; }

// Lets a harness pump the machine's audio synthesis on demand.
extern "C" void host_audio_fill(int32_t *out, int count) {
    if (g_audio_cb) g_audio_cb(out, count);
}

// --- storage -------------------------------------------------------------

struct hal_file { FILE *fp; };

static const char *g_rom_dir = ".";
// extern "C": main.cpp declares it that way (it is harness plumbing, not
// part of the HAL contract, so it gets no header of its own).
extern "C" void host_storage_set_rom_dir(const char *dir) { g_rom_dir = dir; }

bool hal_storage_mount(void) { return true; }
void hal_storage_unmount(void) {}

bool hal_storage_list_dir(const char *dir, hal_storage_dirent_cb cb, void *ctx) {
    // ArcadeMachine_Galaga loads by explicit manifest, not by listing, so
    // this is unused; implemented as a clean failure rather than a lie.
    (void)dir; (void)cb; (void)ctx;
    return false;
}

// Machine code asks for "/rom/<name>"; map that onto the host directory.
hal_file_t *hal_storage_open(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *name  = slash ? slash + 1 : path;
    char full[2048];
    snprintf(full, sizeof(full), "%s/%s", g_rom_dir, name);
    FILE *fp = fopen(full, "rb");
    if (!fp) return NULL;
    hal_file_t *f = (hal_file_t *)malloc(sizeof(hal_file_t));
    if (!f) { fclose(fp); return NULL; }
    f->fp = fp;
    return f;
}

uint32_t hal_storage_read(hal_file_t *f, void *buf, uint32_t len) {
    if (!f || !f->fp) return 0;
    return (uint32_t)fread(buf, 1, len, f->fp);
}

void hal_storage_close(hal_file_t *f) {
    if (!f) return;
    if (f->fp) fclose(f->fp);
    free(f);
}
