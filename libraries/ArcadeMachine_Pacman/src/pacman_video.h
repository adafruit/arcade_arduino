// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Pac-Man tile+sprite VRAM renderer -- board-agnostic (drives ArcadeHAL's
// video contract, never a specific board's display library).
//
// A genuinely different renderer shape from invaders_video.cpp/
// lrescue_video.cpp: those bit-test a raw 1bpp bitmap directly out of game
// RAM; Pac-Man's video hardware is tile+sprite (a 36x28 tilemap of 8x8
// tiles plus 8 hardware sprites), driven by two graphics ROMs and three
// color PROMs that must be decoded first. This file assumes
// HAL_VIDEO_WIDTH/HAL_VIDEO_HEIGHT equal 640x480, same as
// invaders_video.h -- a board with different output geometry would need
// its own renderer variant.
#ifndef PACMAN_VIDEO_H
#define PACMAN_VIDEO_H

#include <stdint.h>
#include "pacman_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

// Raw ROM/PROM staging buffers -- filled by pacman_assets.cpp's loader,
// consumed once by pacman_video_build_caches() to build the decoded
// pixel/palette caches this file actually renders from. Not needed again
// after that (unlike system->rom, which the CPU keeps reading all game
// long).
#define PACMAN_GFX_ROM_SIZE      0x2000 // pacman.5e (tiles) + pacman.5f (sprites)
#define PACMAN_PALETTE_PROM_SIZE 0x0020 // 82s123.7f
#define PACMAN_LOOKUP_PROM_SIZE  0x0100 // 82s126.4a

extern uint8_t pacman_gfx_rom[PACMAN_GFX_ROM_SIZE];
extern uint8_t pacman_palette_prom[PACMAN_PALETTE_PROM_SIZE];
extern uint8_t pacman_lookup_prom[PACMAN_LOOKUP_PROM_SIZE];

// Decodes pacman_gfx_rom/_palette_prom/_lookup_prom (filled by
// pacman_assets.cpp) into this file's internal tile/sprite pixel-index and
// RGB565 palette caches. Call once, after loading and before the first
// first frame is drawn.
void pacman_video_build_caches(void);

// Renders one canvas scanline (dvi_y, 0..HAL_VIDEO_HEIGHT-1 -- the same
// canvas coordinate every renderer here uses; see arcade_hal_video.h)
// into `buf`, honoring system->rotation, system->mirror_x and
// system->flip_screen. Exposed (not static) so pacman_machine.cpp's
// pacman_run_frame() can call it once per scanline, interleaved with the
// Z80 cycles that update the VRAM/sprite state it reads -- see that
// function's own comment (arcade_arduino/DEVNOTES.md problem #19).
//
// EVERY rotation goes through here now. Landscape/180 used to need the
// whole frame's final VRAM state before any scanline could be emitted,
// because a yoko scanline is a native COLUMN and this file could only
// render rows. render_native_column() removed that, and took the 129KB
// frame_cache and the separate whole-frame entry point with it.
void pacman_video_render_scanline(const pacman_system *system, uint32_t dvi_y, uint16_t *buf);

// Boot-time asset-load error screen: floods every scanline with a solid
// color, bypassing game VRAM entirely. Caller loops this forever.
void pacman_draw_error_frame(uint16_t color);

#ifdef __cplusplus
}
#endif

#endif
