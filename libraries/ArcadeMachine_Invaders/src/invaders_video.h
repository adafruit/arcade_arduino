// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Space Invaders VRAM renderer -- board-agnostic (drives ArcadeHAL's video
// contract, never a specific board's display library).
//
// Ported from invaders_pico's pico_video.c. That file's render_scanline()
// math (rotation, mirroring, the tate/yoko border and stretch constants)
// was calibrated against one specific physical display (a 640x480 4:3
// monitor on the Fruit Jam) -- see DEVNOTES.md's "Rendering summary" and
// "Monitor coordinate system" sections in invaders_pico for the derivation.
// This file assumes HAL_VIDEO_WIDTH/HAL_VIDEO_HEIGHT equal 640x480; a board
// with different output geometry would need its own renderer variant (the
// math itself doesn't generalize, only the HAL calls it's built on do).
#ifndef INVADERS_VIDEO_H
#define INVADERS_VIDEO_H

#include "invaders_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

// Renders and submits one full frame (HAL_VIDEO_HEIGHT scanlines) from
// `system`'s VRAM, honoring system->rotation and system->mirror_x.
//
// NOTE: invaders_run_frame() no longer calls this -- it drives the scanline
// pump itself via invaders_video_render_scanline() below so CPU execution
// can be interleaved with scanline output (DEVNOTES.md problem #34). This
// remains for any caller that genuinely wants a standalone frame with no
// CPU to interleave.
void invaders_draw_frame(arcade_system *system);

// Renders ONE physical scanline (dvi_y) into `buf`, without acquiring or
// submitting it. Exposed so invaders_run_frame() can own the
// acquire/render/submit pump and spread the frame's i8080 cycles across it
// -- see DEVNOTES.md problem #34 for why running a whole frame's emulation
// before submitting the first scanline starves Core 1's DVI queue, and
// problem #20 for the same fix in Pac-Man.
void invaders_video_render_scanline(uint32_t dvi_y, uint16_t *buf, const arcade_system *system);

// Boot-time asset-load error screen: floods every scanline with a solid
// color, bypassing game VRAM entirely. Caller loops this forever; the
// display needs continuous feeding to keep a signal alive at all.
void invaders_draw_error_frame(uint16_t color);

#ifdef __cplusplus
}
#endif

#endif
