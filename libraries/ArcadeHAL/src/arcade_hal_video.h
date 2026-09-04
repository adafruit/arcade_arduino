// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// ArcadeHAL: video contract.
//
// A board backend implements this by generating a continuous stream of
// scanlines for its physical display. It does NOT know about game VRAM,
// rotation, or mirroring -- that's Machine-layer logic (see
// ArcadeMachine_Invaders's invaders_video.*). This contract only knows how
// to move filled RGB565 pixel rows onto a screen.
//
// Exactly one ArcadeBoard_* library implements this per build (SAMP: no
// runtime board selection), so these are plain C functions, not a vtable --
// zero indirection cost.
#ifndef ARCADE_HAL_VIDEO_H
#define ARCADE_HAL_VIDEO_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// The DRAWABLE CANVAS, in logical pixels: how many pixels a caller fills
// per scanline, and how many scanlines make up one frame. On Fruit Jam this
// is 320x240, and one logical pixel is displayed as a 2x2 block of physical
// pixels -- but that is the board's business, not the renderer's. A machine
// renderer only ever works in canvas coordinates: x in 0..HAL_VIDEO_WIDTH-1,
// scanline index in 0..HAL_VIDEO_HEIGHT-1.
//
// THESE USED TO BE THE PHYSICAL RESOLUTION (640x480), WITH A SEPARATE
// `HAL_VIDEO_SCANLINES_PER_FRAME` (240) FOR THE SUBMISSION COUNT -- a third
// constant, now removed, that existed only to paper over the mismatch.
// That mismatch caused a real, long-lived bug. Renderers were handed a y in
// 0..479 of which only even values ever arrived, while x ran 0..319 --
// two coordinate systems for one canvas, bridged by a x2 or /2 hidden in
// every rotation formula. When DEVNOTES #10 halved the submission count it
// preserved the border constants, which are POSITIONS, but silently changed
// the meaning of landscape's `/ HAL_VIDEO_HEIGHT`, which is a RESAMPLING
// RATIO -- turning a lossless upsample into a downsample that drops 16 of
// 256 (or 48 of 288) native lines. See DEVNOTES #23, #75 and #76, and
// DISPLAY_GEOMETRY.md.
//
// One coordinate space makes that class of bug unrepresentable, which is
// the entire reason these are declared this way. A board whose display
// repeats or scales submitted scanlines reports the CANVAS here and keeps
// the repeat factor to itself.
extern const uint32_t HAL_VIDEO_WIDTH;
extern const uint32_t HAL_VIDEO_HEIGHT;

// One-time setup: allocate buffers and configure the display peripheral.
// MUST NOT start driving the physical signal yet -- see hal_video_run().
// Returns false on failure (e.g. buffer allocation).
bool hal_video_init(void);

// Acquire a scanline buffer to fill (blocks until one is free). The buffer
// holds HAL_VIDEO_WIDTH RGB565 pixels; caller fills all of them.
uint16_t *hal_video_acquire_scanline(void);

// Submit a filled scanline buffer for display. Callers must submit exactly
// once per hal_video_acquire_scanline(), HAL_VIDEO_HEIGHT times per frame,
// repeating forever. There is no separate "y" parameter:
// the board backend tracks scan position internally from call order,
// matching the reference clone's queue design.
void hal_video_submit_scanline(uint16_t *buf);

// Entry point for whatever execution context actually drives display
// timing (e.g. the second core on an RP2350's HSTX/DVI peripheral). Never
// returns. Call only after hal_video_init(), and only once the caller is
// ready to keep feeding scanlines continuously via acquire/submit --
// starting this any earlier starves the display and shows as a glitch or
// blank screen (see invaders_pico's DEVNOTES.md, "Frame timing hang" and
// "Boot-time error screen" sections for why this ordering matters).
void hal_video_run(void);

// Microseconds spent BLOCKED inside hal_video_acquire_scanline() since the
// previous call to this function, then resets the counter.
//
// hal_video_acquire_scanline() blocks until the display backend frees a
// buffer, so a game timing its own frame loop measures max(work, display
// frame period): once the work fits the budget the measured time pins at
// the refresh period and hides how much headroom is actually left.
// Subtract this to get the real work time. Board backends with no such
// blocking may return 0.
uint32_t hal_video_take_blocked_us(void);

// Number of times, since the last call, that submitting a scanline left the
// display backend with almost nothing queued -- i.e. how often the producer
// came within a hair of starving the output. Resets on read.
//
// This exists because DVI starvation is the one failure in this project that
// is loud on screen (red bars or a fully red frame) and completely invisible
// to every other instrument: `work` can sit comfortably inside the frame
// budget while an uneven patch inside that frame drains the queue anyway
// (DEVNOTES.md #35). A board with no such failure mode may return 0.
uint32_t hal_video_take_starve_count(void);

// Lowest VALID-queue level seen since the last call, and the total number of
// scanline buffers. min_valid is the pipeline's remaining runway at its
// worst moment: 0 means the display ran dry (a red line), and a value near
// the buffer count means the queue never came close. Prefer this over any
// run-of-non-blocking-acquires heuristic, which is only meaningful when the
// free and valid queues are complementary -- see DEVNOTES #85.
uint32_t hal_video_take_min_valid_level(void);
uint32_t hal_video_scanbuf_count(void);

#ifdef __cplusplus
}
#endif

#endif
