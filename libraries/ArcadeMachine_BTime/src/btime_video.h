// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Burger Time char+sprite+background renderer -- board-agnostic (drives
// ArcadeHAL's video contract, never a specific board's display library).
// Assumes HAL_VIDEO_WIDTH/HAL_VIDEO_HEIGHT are 640x480 with 240 submitted
// scanlines, same as its siblings.
#ifndef BTIME_VIDEO_H
#define BTIME_VIDEO_H

#include <stdint.h>
#include "btime_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

// Raw graphics-ROM staging buffers, filled by btime_assets.cpp. Unlike
// ArcadeMachine_Pacman's, these are NOT consumed once and forgotten -- this
// renderer reads the planar ROM bytes directly every frame (see
// btime_video.cpp's header for why that is cheaper here than a decode
// cache), so they stay live for the life of the machine.
#define BTIME_GFX1_SIZE   0x6000 // chars AND sprites: 3 planes x 0x2000
#define BTIME_GFX2_SIZE   0x1800 // background tiles:  3 planes x 0x0800
#define BTIME_BG_MAP_SIZE 0x0800 // ab03.6b: 8 pages x 256 tile numbers

extern uint8_t btime_gfx1[BTIME_GFX1_SIZE];
extern uint8_t btime_gfx2[BTIME_GFX2_SIZE];
extern uint8_t btime_bg_map[BTIME_BG_MAP_SIZE];

// Builds the bit-spread lookup table this renderer uses instead of decode
// caches. Call once at load time; cheap and idempotent.
void btime_video_build_caches(void);

// Called from btime_ports.cpp on every write to palette RAM (0x0C00-0x0C0F)
// to rebuild that entry's RGB565 value. The palette on this board is RAM
// the game writes, not a PROM decoded at boot, so this is a hot-ish path
// rather than a one-off.
void btime_video_palette_write(uint8_t index, uint8_t value);

// Renders and submits one full frame. Used only for landscape/180 rotation
// -- see btime_video_render_scanline() for why tate drives its own loop.
void btime_draw_frame(btime_system *system);

// Renders one canvas scanline (dvi_y, 0..HAL_VIDEO_HEIGHT-1 -- the same
// canvas coordinate every renderer here uses; see arcade_hal_video.h)
// into `buf`. Exposed so btime_machine.cpp can call it once per
// scanline interleaved with CPU execution.
void btime_video_render_scanline(const btime_system *system, uint32_t dvi_y,
                                 uint16_t *buf);

// Boot-time asset-load error screen: floods every scanline with a solid
// colour, bypassing game VRAM entirely. Caller loops this forever.
void btime_draw_error_frame(uint16_t color);

// When true, tate mode stretches the game's vertical axis across all 320
// visible framebuffer columns instead of centring it 1:1 in 240 of them.
// See btime_video.cpp's geometry comment: 1:1 is what every other game here
// does, but this raster is square, so 1:1 shows the picture at 3/4 of its
// correct height -- the largest aspect error in the project. Default is the
// stretch (correct); flip it to compare on hardware.
void btime_video_set_aspect_stretch(bool enable);
bool btime_video_get_aspect_stretch(void);

#ifdef __cplusplus
}
#endif

#endif
