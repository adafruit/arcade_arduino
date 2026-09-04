// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Shared PPM frame dumper for the host harnesses. See host_ppm.h for the
// interface and ../README.md for how the harnesses use it.
//
// WHAT THE SIX PRIVATE COPIES OF THIS GOT WRONG, and why it mattered.
//
// Every harness's dump_ppm() looped `y = 0 .. HAL_VIDEO_HEIGHT-1`, i.e. it
// rendered EVERY physical row -- 480 calls per frame. The device submits
// HAL_VIDEO_SCANLINES_PER_FRAME (240) and lets the DVI peripheral's
// `dvi_vertical_repeat = 2` show each one on two consecutive physical rows
// (DEVNOTES #10).
//
// For tate that difference is invisible, because tate's row formula divides
// dvi_y by 2 and lands on the same native row either way. For LANDSCAPE it
// is the whole bug: landscape's `dy = dvi_y * GAME_WIDTH / HAL_VIDEO_HEIGHT`
// is a resampling ratio, and at 480 samples it maps all 256 (or 288) source
// columns onto the screen with NONE dropped, while at the device's real 240
// samples it drops 16 of them (48 for the Namco games) -- evenly spaced,
// every 16th or every 6th, but never drawn at all. See DEVNOTES #76.
//
// So the harnesses rendered landscape at the PRE-#10 sample rate: they drew
// it the lossless way and structurally could not reproduce the defect at
// all. Any geometry change validated against those dumps was validated
// against a model that did not contain the bug. See DISPLAY_GEOMETRY.md
// section 6.
//
// WHAT THIS ONE DOES INSTEAD, and why the output is still 640x480:
//
//   - Renders exactly HAL_VIDEO_SCANLINES_PER_FRAME times, at
//     `i * (HAL_VIDEO_HEIGHT / HAL_VIDEO_SCANLINES_PER_FRAME)` -- the same
//     coordinate the board backend passes, per arcade_hal_video.h.
//   - Writes each rendered row `step` times, reproducing the vertical
//     repeat rather than re-rendering it.
//   - Reads only `buf[x / 2]`, reproducing the HORIZONTAL doubling, which
//     is the half that never made it into the HAL: libdvi's
//     _dvi_prepare_scanline_16bpp() passes `pixwidth / 2` to the TMDS
//     encoder, so it reads only buf[0..319] and doubles it across the
//     640-pixel line. Four of the six copies already did this; galaga_host
//     and pacman_host did not, and their dumps put the picture in the left
//     half against black.
//
// The file therefore stays 640x480 and looks like the monitor. Two
// properties are worth asserting on if you ever doubt a dump: every pair of
// adjacent output rows is identical, and every pair of adjacent output
// columns is identical. Both hold here and neither held before.
#include "host_ppm.h"
#include "arcade_hal_video.h"

#include <stdio.h>
#include <string.h>

bool host_ppm_write(const char *path, host_ppm_render_fn render, void *ctx) {
    FILE *fp = fopen(path, "wb");
    if (!fp) { fprintf(stderr, "cannot write %s\n", path); return false; }

    // One submitted scanline covers this many physical rows. The board
    // backend's own contract (arcade_hal_video.h) states HAL_VIDEO_HEIGHT
    // always divides evenly by HAL_VIDEO_SCANLINES_PER_FRAME.
    const uint32_t step = HAL_VIDEO_HEIGHT / HAL_VIDEO_SCANLINES_PER_FRAME;

    static uint16_t row[4096];
    static uint8_t  out[4096 * 3];

    fprintf(fp, "P6\n%u %u\n255\n", HAL_VIDEO_WIDTH, HAL_VIDEO_HEIGHT);

    for (uint32_t i = 0; i < HAL_VIDEO_SCANLINES_PER_FRAME; i++) {
        memset(row, 0, sizeof(uint16_t) * HAL_VIDEO_WIDTH);
        render(ctx, i * step, row);

        for (uint32_t x = 0; x < HAL_VIDEO_WIDTH; x++) {
            // Horizontal pixel doubling -- see this file's header.
            const uint16_t c = row[x >> 1];
            out[x * 3u + 0u] = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
            out[x * 3u + 1u] = (uint8_t)(((c >>  5) & 0x3F) * 255 / 63);
            out[x * 3u + 2u] = (uint8_t)(( c        & 0x1F) * 255 / 31);
        }

        // Vertical repeat: the same submitted scanline on `step` physical
        // rows, which is what dvi_vertical_repeat = 2 does on the board.
        for (uint32_t r = 0; r < step; r++)
            fwrite(out, 1, (size_t)HAL_VIDEO_WIDTH * 3u, fp);
    }

    fclose(fp);
    return true;
}
