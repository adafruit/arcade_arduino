// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// ArcadeHAL: storage contract.
//
// A minimal, filesystem-shaped primitive modeled loosely on stdio: mount,
// list a directory, open/read/close a file. A board backend can back this
// with an SD card (as Fruit Jam does), onboard flash, or anything else --
// Machine code (asset manifests, ROM/WAV loading) only ever calls through
// this contract, never touches a filesystem library directly.
#ifndef ARCADE_HAL_STORAGE_H
#define ARCADE_HAL_STORAGE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hal_file hal_file_t; // opaque, defined by the board backend

// Mount whatever backing store this board uses. Returns false if no card/
// device is present at all -- callers use this to distinguish "no storage"
// from "storage present but empty/wrong contents" (see
// ArcadeMachine_Invaders's invaders_assets.h for the boot-error-color
// distinction this enables).
bool hal_storage_mount(void);

// Unmount. Call once all loading is done; the reference design never
// touches storage again after boot (everything needed is copied to RAM).
void hal_storage_unmount(void);

// Calls cb(filename, ctx) once per regular file (not subdirectory) found
// directly inside `dir`. Returns false if the directory itself couldn't be
// opened.
typedef void (*hal_storage_dirent_cb)(const char *filename, void *ctx);
bool hal_storage_list_dir(const char *dir, hal_storage_dirent_cb cb, void *ctx);

// Returns NULL on failure (file missing, open error, etc).
hal_file_t *hal_storage_open(const char *path);

// Reads up to `len` bytes into `buf`. Returns the number of bytes actually
// read (0 at EOF or on error).
uint32_t hal_storage_read(hal_file_t *f, void *buf, uint32_t len);

void hal_storage_close(hal_file_t *f);

#ifdef __cplusplus
}
#endif

#endif
