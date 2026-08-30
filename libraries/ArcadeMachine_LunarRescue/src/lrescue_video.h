// Lunar Rescue VRAM+color-PROM renderer -- board-agnostic (drives
// ArcadeHAL's video contract, never a specific board's display library).
//
// Structurally a copy of ArcadeMachine_Invaders' invaders_video.cpp (same
// rotation/mirror/tate-vs-landscape math, calibrated against the same
// 640x480 4:3 monitor -- see that file and invaders_pico's DEVNOTES.md
// "Rendering summary" for the derivation), but recolors each 8x8-pixel
// block from the color-map PROM instead of drawing fixed white-on-black:
// Lunar Rescue's PCB has real per-block hardware color (an RGB_3BIT
// palette driven by 7643-1.cpu), unlike Space Invaders' monochrome CRT
// with a plastic color overlay taped over it.
#ifndef LRESCUE_VIDEO_H
#define LRESCUE_VIDEO_H

#include "lrescue_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

// Renders and submits one full frame (HAL_VIDEO_HEIGHT scanlines) from
// `system`'s VRAM, honoring system->rotation, system->mirror_x, and
// system->screen_red.
void lrescue_draw_frame(arcade_system *system);

// Renders ONE physical scanline (dvi_y) into `buf`. Exposed so
// lrescue_run_frame() can drive the scanline pump itself, interleaved with
// CPU execution, instead of running a whole frame's emulation and only then
// calling lrescue_draw_frame() -- see DEVNOTES.md problem #34 for why that
// old shape caused red lines, and problem #20 for the same fix in Pac-Man.
// lrescue_draw_frame() above remains for callers that genuinely want a
// standalone frame with no CPU to interleave (the sketch's self-test).
void lrescue_video_render_scanline(uint32_t dvi_y, uint16_t *buf, const arcade_system *system);

// Boot-time asset-load error screen: floods every scanline with a solid
// color, bypassing game VRAM entirely.
void lrescue_draw_error_frame(uint16_t color);

// DEBUG: the last lrescue_draw_frame() call's total time, split into pure
// render *compute* (this file's own CPU-bound work) vs. time spent blocked
// inside hal_video_acquire_scanline()/hal_video_submit_scanline() waiting
// on Core 1 -- see lrescue_draw_frame()'s doc comment for why that split,
// not a single "how long did drawing take" number, is what's actually
// diagnostic here. Safe to delete once the red-scanline investigation is
// resolved.
void lrescue_video_debug_last_frame_us(uint32_t *render_us, uint32_t *block_us);

#ifdef __cplusplus
}
#endif

#endif
