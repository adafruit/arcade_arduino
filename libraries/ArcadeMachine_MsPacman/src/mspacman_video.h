// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Ms. Pac-Man tile+sprite VRAM renderer -- board-agnostic (drives ArcadeHAL's
// video contract, never a specific board's display library).
//
// IDENTICAL to ArcadeMachine_Pacman's pacman_video.h: Ms. Pac-Man is the
// same board and the same video hardware, and its aux daughterboard adds
// no video of its own. Only the graphics ROM FILENAMES differ ("5e"/"5f"
// rather than "pacman.5e"/"pacman.5f"), which is a manifest concern and
// lives in mspacman_assets.cpp, not here.
// ORIGINAL COMMENT FOLLOWS, unchanged:
//
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
#ifndef MSPACMAN_VIDEO_H
#define MSPACMAN_VIDEO_H

#include <stdint.h>
#include "mspacman_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

// Raw ROM/PROM staging buffers -- filled by mspacman_assets.cpp's loader,
// consumed once by mspacman_video_build_caches() to build the decoded
// pixel/palette caches this file actually renders from. Not needed again
// after that (unlike system->rom, which the CPU keeps reading all game
// long).
#define MSPACMAN_GFX_ROM_SIZE      0x2000 // pacman.5e (tiles) + pacman.5f (sprites)
#define MSPACMAN_PALETTE_PROM_SIZE 0x0020 // 82s123.7f
#define MSPACMAN_LOOKUP_PROM_SIZE  0x0100 // 82s126.4a

extern uint8_t mspacman_gfx_rom[MSPACMAN_GFX_ROM_SIZE];
extern uint8_t mspacman_palette_prom[MSPACMAN_PALETTE_PROM_SIZE];
extern uint8_t mspacman_lookup_prom[MSPACMAN_LOOKUP_PROM_SIZE];

// Decodes mspacman_gfx_rom/_palette_prom/_lookup_prom (filled by
// mspacman_assets.cpp) into this file's internal tile/sprite pixel-index and
// RGB565 palette caches. Call once, after loading and before the first
// mspacman_draw_frame().
void mspacman_video_build_caches(void);

// Renders and submits one full frame (HAL_VIDEO_HEIGHT scanlines) from
// `system`'s video/color/sprite RAM, honoring system->rotation,
// system->mirror_x and system->flip_screen. Used directly only for
// landscape/180-degree rotation now -- see mspacman_video_render_scanline()
// below for why tate/CW drives its own loop instead.
void mspacman_draw_frame(mspacman_system *system);

// Renders one canvas scanline (dvi_y, 0..HAL_VIDEO_HEIGHT-1 -- the same
// canvas coordinate every renderer here uses; see arcade_hal_video.h)
// into `buf`. Exposed (not static) so mspacman_machine.cpp's
// mspacman_run_frame() can call it once per scanline, interleaved with the
// Z80 cycles that update the VRAM/sprite state it reads -- see that
// function's own comment (arcade_arduino/DEVNOTES.md problem #19) for why
// this matters for tate/CW rotation specifically. Landscape/180-degree
// rotation still needs the WHOLE frame's final VRAM state before any
// scanline can be emitted (see mspacman_video.cpp's frame_cache comment),
// so mspacman_run_frame() never calls this directly for those two modes --
// it uses mspacman_draw_frame() instead, after running the full frame's
// cycles.
void mspacman_video_render_scanline(const mspacman_system *system, uint32_t dvi_y, uint16_t *buf);

// Boot-time asset-load error screen: floods every scanline with a solid
// color, bypassing game VRAM entirely. Caller loops this forever.
void mspacman_draw_error_frame(uint16_t color);

#ifdef __cplusplus
}
#endif

#endif
