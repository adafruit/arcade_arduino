// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Shared screen geometry: where a game's raster lands on the canvas, and
// how it is resampled to get there. One source of truth for all seven
// machines. See DISPLAY_GEOMETRY.md for the derivation.
//
// WHAT THIS REPLACES. Every renderer carried its own hand-derived
// TATE_BX/TATE_BY/LAND_BX constants and its own inline resampling
// arithmetic, four rotation cases at a time. That is 32 hand-derived cases
// across the project, each verified separately, and it is what produced
// DEVNOTES #21 (a mirror instead of a rotation), #33 (a default rotation
// with no fast path) and #23 (the landscape resampling). All three are the
// same failure: a case derived from a sibling case rather than from one
// source of truth.
//
// THE GEOMETRY, in one place.
//
// The canvas is 320x240 logical SQUARE pixels on a 4:3 panel
// (arcade_hal_video.h). Name a game's raster by its axes rather than by
// "width" and "height", which are ambiguous once a cabinet is rotated:
//
//   LONG  axis -- the raster axis that runs VERTICALLY on a real rotated
//                 cabinet's tube. 256 for Invaders/Lunar Rescue/DKong, 288
//                 for the Namco games, 240 for Burger Time. This is the
//                 dimension those machines call *_GAME_WIDTH.
//   SHORT axis -- the other one. 224, or 240 for Burger Time.
//                 Called *_GAME_HEIGHT.
//
// A real cabinet fills its tube, so the displayed picture is 4 units along
// LONG by 3 along SHORT. The canvas is 4 units by 3. Therefore:
//
//   TATE (monitor physically rotated): LONG -> all 320 canvas columns,
//        SHORT -> all 240 canvas rows. The picture fills the screen.
//   YOKO (monitor upright, portrait game shown inside it): LONG is capped
//        by the 240 canvas rows, so SHORT must be 240 * 3/4 = 180 columns,
//        pillarboxed 70 each side.
//
// **BOTH DESTINATIONS ARE THE SAME FOR EVERY GAME** -- 320x240 and 180x240.
// Only the source raster differs, so only the resampling ratio is
// per-game. That is why this can be one module rather than seven.
//
// YOKO NEEDS THE PICTURE NARROWER, NOT TALLER. This is the one most likely
// to be got backwards: the old code drew SHORT at 1:1 (224 columns) where
// 180 is correct, which is the whole of landscape's 24.4% error.
//
// COST. Resampling is a table lookup per pixel, built once at init. No
// division and no branch in any inner loop -- which matters, because Donkey
// Kong runs at ~15.5ms of a 16.66ms budget and tate's 224->240 upsample
// makes 16 canvas rows repeat a source row (see av_map_t::row and the
// memoisation note on it).
#ifndef ARCADE_VIDEO_GEOM_H
#define ARCADE_VIDEO_GEOM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Array bounds. These are the canvas size this module is compiled for; a
// board with a larger canvas needs them raised. av_geom_init() clamps to
// the board's actual HAL_VIDEO_WIDTH/HEIGHT at runtime.
#define AV_CANVAS_W 320u
#define AV_CANVAS_H 240u

// Aspect-correct SHORT-axis extent in yoko: 240 * 3/4. Independent of the
// game -- see this file's header.
#define AV_YOKO_W   180u

// One orientation's mapping from canvas coordinates to raster indices.
//
// `col`/`row` are only meaningful inside [x0,x1) and [y0,y1); outside those
// the canvas is border and the renderer clears it. Keeping the borders as
// RANGES rather than as a sentinel inside the tables is deliberate: a
// sentinel would put a branch in a loop that runs 76,800 times a frame.
typedef struct {
    uint16_t x0, x1;            // canvas columns carrying picture, [x0, x1)
    uint16_t y0, y1;            // canvas rows carrying picture,    [y0, y1)
    uint16_t col[AV_CANVAS_W];  // canvas x -> raster index along one axis
    uint16_t row[AV_CANVAS_H];  // canvas y -> raster index along the other

    // True when `col` is the identity shift, i.e. col[x0 + i] == i. A
    // renderer that can write its raster row STRAIGHT into the scanline
    // buffer (galaga_video.cpp's tate fast path does exactly this) may take
    // that shortcut only when this is set; otherwise it must render to a
    // scratch row and map through `col`.
    //
    // This is a real performance fork, not a tidiness one. DEVNOTES #33:
    // when Galaga's default rotation changed to one WITHOUT a direct path,
    // the extra clear-and-copy per scanline was instantly enough to blow
    // its ~3ms of headroom and put red bars back on a real screen. Galaga
    // peaks at 14946us of a 16660us budget, so the shortcut is load-bearing
    // at that game's default.
    uint8_t  col_1to1;
} av_map_t;

// Tate (rotations 1 and 3): col indexes the LONG axis, row the SHORT axis.
extern av_map_t av_tate;
// Yoko (rotations 0 and 2): col indexes the SHORT axis, row the LONG axis.
extern av_map_t av_yoko;

// Build both maps for a game's raster. Call once from the machine's init,
// before the first frame. `long_px` is *_GAME_WIDTH, `short_px` is
// *_GAME_HEIGHT.
void av_geom_init(uint32_t long_px, uint32_t short_px);

// Aspect-ratio correction on/off, rebuilding both maps. OFF reproduces the
// project's historical 1:1-plus-pillarbox layout EXACTLY, pixel for pixel,
// which is what makes an A/B on real hardware a single button press and
// what lets the host harness prove this module changed nothing when off.
//
// Default is OFF. Turning it on is a visual change to every game and should
// be compared on the physical display before it becomes the shipped
// default -- the same discipline btime_video.cpp already used for its own
// one-game version of this.
void av_geom_set_stretch(bool on);
bool av_geom_get_stretch(void);

#ifdef __cplusplus
}
#endif

#endif
