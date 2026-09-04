// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Galaga tile+sprite VRAM renderer -- board-agnostic (drives ArcadeHAL's
// video contract, never a specific board's display library).
//
// Every hardware fact below was verified against MAME's galaga_v.cpp
// (src/mame/namco/galaga_v.cpp, fetched and read directly this session --
// see galaga_machine.h's header comment for the full citation trail), NOT
// assumed to match ArcadeMachine_Pacman's renderer despite the visible
// similarities (same 288x224 raster, same 36x28 tilemap addressing
// formula, same tile bit-packing convention, same palette resistor-ladder
// values) -- Galaga's SPRITES are meaningfully different hardware: up to
// 64 sprites, each independently 16x16/16x32/32x16/32x32 pixels (1 or 2
// cells per axis, via a "gfx_offs" quadrant-select table), not Pac-Man's
// fixed 8 sprites at a fixed 16x16. This file's sprite renderer reflects
// that real difference; do not simplify it back to Pac-Man's shape.
//
// This file assumes HAL_VIDEO_WIDTH/HAL_VIDEO_HEIGHT equal 640x480, same
// as pacman_video.h -- a board with different output geometry would need
// its own renderer variant.
//
// All three layers are implemented, in galaga_v.cpp's screen_update_galaga()
// draw order: starfield -> sprites -> tilemap. The starfield is the Namco
// 05XX, driven by the six videolatch bits galaga_ports.cpp captures into
// galaga_system::starfield_control; galaga_video.cpp's own header block
// covers the LFSR derivation and, importantly, why this implementation does
// NOT clock that LFSR per pixel the way MAME does.
#ifndef GALAGA_VIDEO_H
#define GALAGA_VIDEO_H

#include <stdint.h>
#include "galaga_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

// Raw ROM/PROM staging buffers -- filled by galaga_assets.cpp's loader,
// consumed once by galaga_video_build_caches() to build the decoded
// pixel/palette caches this file actually renders from.
#define GALAGA_GFX1_SIZE          0x1000 // gg1-9.4l -- tiles, 256 x 16 bytes
#define GALAGA_GFX2_SIZE          0x2000 // gg1-11.4d + gg1-10.4f -- sprites, 128 x 64 bytes
#define GALAGA_PALETTE_PROM_SIZE  0x0020 // prom-5.5n -- 32x8 core palette
#define GALAGA_CHAR_LOOKUP_SIZE   0x0100 // prom-4.2n -- character color lookup
#define GALAGA_SPRITE_LOOKUP_SIZE 0x0100 // prom-3.1c -- sprite color lookup

extern uint8_t galaga_gfx1_rom[GALAGA_GFX1_SIZE];
extern uint8_t galaga_gfx2_rom[GALAGA_GFX2_SIZE];
extern uint8_t galaga_palette_prom[GALAGA_PALETTE_PROM_SIZE];
extern uint8_t galaga_char_lookup_prom[GALAGA_CHAR_LOOKUP_SIZE];
extern uint8_t galaga_sprite_lookup_prom[GALAGA_SPRITE_LOOKUP_SIZE];

// Decodes the 5 staging buffers above into this file's internal
// tile/sprite pixel-index and RGB565 palette caches. Call once, after
// loading and before the first galaga_draw_frame().
void galaga_video_build_caches(void);

// Decodes this frame's sprite list once, up front. MUST be called at the
// start of every frame, before any galaga_video_render_scanline() call for
// that frame -- the per-scanline renderer consumes what this produces and
// will otherwise draw the previous frame's sprites. See the long comment
// on galaga_video_begin_frame() in galaga_video.cpp for why sprites are
// latched per frame rather than re-read per scanline (it is a real frame-
// budget fix on the Fruit Jam, and it does trade away mid-frame sprite
// register changes).
void galaga_video_begin_frame(const galaga_system *system);

// Renders one physical scanline (dvi_y). Exposed so galaga_machine.cpp's
// galaga_run_frame() can call it once per scanline, interleaved with CPU
// execution, same rationale pacman_machine.cpp documents (DEVNOTES.md
// problem #19) for tate/CW rotation.
void galaga_video_render_scanline(const galaga_system *system, uint32_t dvi_y, uint16_t *buf);

// Boot-time asset-load error screen: floods every scanline with a solid
// color, bypassing game VRAM entirely.
void galaga_draw_error_frame(uint16_t color);

// On-screen sprite count from the last decoded frame -- see the sketch's
// frame-budget heartbeat.
uint32_t galaga_video_debug_sprite_count(void);

#ifdef __cplusplus
}
#endif

#endif
