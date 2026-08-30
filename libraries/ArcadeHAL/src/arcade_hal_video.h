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

// Output geometry, defined by the board backend (e.g. 640x480 for Fruit
// Jam's DVI output). Machine renderers read these rather than hardcoding a
// resolution, so the same renderer file at least *compiles* against a board
// with different geometry (the rotation/scaling math itself is still tuned
// per physical display -- see invaders_video.c's border constants).
extern const uint32_t HAL_VIDEO_WIDTH;
extern const uint32_t HAL_VIDEO_HEIGHT;

// Number of hal_video_acquire_scanline()/hal_video_submit_scanline() calls
// that make up one full frame. This can be LESS than HAL_VIDEO_HEIGHT if
// the board's display peripheral internally repeats each submitted
// scanline across more than one physical output row (e.g. Fruit Jam's DVI
// peripheral currently only works reliably at a 2x vertical repeat -- see
// hal_video_fruitjam.cpp for why). HAL_VIDEO_HEIGHT always divides evenly
// by this. Callers computing per-scanline content should pass
// `i * (HAL_VIDEO_HEIGHT / HAL_VIDEO_SCANLINES_PER_FRAME)` as the
// effective physical Y coordinate for the i'th call of a frame.
extern const uint32_t HAL_VIDEO_SCANLINES_PER_FRAME;

// One-time setup: allocate buffers and configure the display peripheral.
// MUST NOT start driving the physical signal yet -- see hal_video_run().
// Returns false on failure (e.g. buffer allocation).
bool hal_video_init(void);

// Acquire a scanline buffer to fill (blocks until one is free). The buffer
// holds HAL_VIDEO_WIDTH RGB565 pixels; caller fills all of them.
uint16_t *hal_video_acquire_scanline(void);

// Submit a filled scanline buffer for display. Callers must submit exactly
// once per hal_video_acquire_scanline(), HAL_VIDEO_SCANLINES_PER_FRAME
// times per frame, repeating forever. There is no separate "y" parameter:
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

#ifdef __cplusplus
}
#endif

#endif
