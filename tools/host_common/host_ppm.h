// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Shared PPM frame dumper for the host harnesses -- see ../README.md's
// "PPM dumps and what the monitor actually shows".
//
// This exists as ONE function rather than six copies because the six copies
// were all subtly wrong in the same way, and a bug in a measuring
// instrument is worse than a bug in the thing being measured: it does not
// look like a defect, it looks like evidence. See host_ppm.cpp's header for
// what they got wrong.
#ifndef HOST_PPM_H
#define HOST_PPM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Renders one scanline for physical row `dvi_y` into `buf`, which holds
// HAL_VIDEO_WIDTH RGB565 pixels. Same contract the board backend imposes on
// a machine renderer, so the trampoline in each harness is a one-line cast.
typedef void (*host_ppm_render_fn)(void *ctx, uint32_t dvi_y, uint16_t *buf);

// Renders a whole frame through `render` and writes it as a binary PPM at
// `path`, reproducing what the physical display would show. Returns false
// (and prints to stderr) if the file cannot be opened.
bool host_ppm_write(const char *path, host_ppm_render_fn render, void *ctx);

#ifdef __cplusplus
}
#endif

#endif
