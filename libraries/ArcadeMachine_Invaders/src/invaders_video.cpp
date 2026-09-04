// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Space Invaders VRAM renderer -- ported from invaders_pico's pico_video.c
// render_scanline()/draw_frame()/draw_error_frame(). Hardware bring-up
// (DVI/HSTX init, the Core 1 scanline pump) is gone from this file; it
// lives in the board backend behind hal_video_init()/hal_video_run(). This
// file keeps only the VRAM-to-pixels math, which is game-specific (a
// different machine, e.g. a tile+sprite game, would need a completely
// different renderer) but board-agnostic (it only calls hal_video_*).
#include <string.h>
#include "invaders_video.h"
#include "arcade_hal_video.h"
#include "arcade_video_geom.h"

// Where the picture lands on the canvas, and how it is resampled to get
// there, comes from ArcadeHAL's arcade_video_geom.h -- av_tate and av_yoko,
// built by av_geom_init(INVADERS_GAME_WIDTH, INVADERS_GAME_HEIGHT) in
// invaders_init(). This file used to carry its own TATE_BX/TATE_BY/LAND_BX,
// and every other renderer in the project was derived from those constants
// by hand; that copying is what produced DEVNOTES #21, #23 and #33.
//
// Read arcade_video_geom.h before touching the loops below.

#define COLOR_WHITE 0xFFFFu

// VRAM layout: column-major, dx=0..223 left->right, dy=0..255 top(score)->bottom(player).
//   byte = memory[0x2400 + dx*32 + (255-dy)/8],  bit = (255-dy)%8

static void render_scanline(uint32_t dvi_y, uint16_t *buf, const arcade_system *sys) {
    memset(buf, 0, HAL_VIDEO_WIDTH * sizeof(uint16_t));
    const uint8_t *vram = sys->state.memory + 0x2400;
    bool mir = sys->mirror_x;

    switch (sys->rotation) {

    case 0: {
        // Landscape. av_yoko.row maps the canvas row onto the raster's LONG
        // axis (dy); av_yoko.col maps canvas columns onto the SHORT axis --
        // and in yoko the SHORT axis is the one that must NARROW to 180
        // canvas columns, not sit at 1:1. See arcade_video_geom.h.
        // MERGED, both axes. Yoko DOWNSAMPLES both of them -- the long axis
        // 256 -> 240 canvas rows always, and the short axis 224 -> 180 when
        // the aspect correction is on -- and the loop this replaced was
        // destination-driven nearest-neighbour: it read one source sample
        // per canvas pixel and never looked at the rest. On 1-pixel-wide
        // line art that does not thin a feature, it DELETES it. On a real
        // screen the score line read "SC.NRF<1> HT-SC.NRF" uncorrected and
        // "SC7BF(1> 4|-SrnRF" corrected -- whole letter strokes gone.
        //
        // Same rule and same fix as Pac-Man's (DEVNOTES #80): walk the
        // SOURCE and let a lit sample win, so a collapsed feature is
        // thickened by a pixel rather than lost. This game is 1-bit
        // white-on-black, so "merge" is simply OR.
        //
        // Note this helps with the correction OFF too: yoko's long axis is
        // resampled 256 -> 240 in BOTH modes (arcade_video_geom.h's
        // rebuild()), which is where the uncorrected damage came from.
        const uint32_t r0   = av_yoko.row[dvi_y];
        const uint32_t nrow = av_yoko.rowrep[dvi_y] ? av_yoko.rowrep[dvi_y] : 1u;
        const uint32_t n    = av_yoko.src_n;
        const uint8_t *rep  = av_yoko.rep;
        uint32_t x = av_yoko.x0;
        for (uint32_t sx = 0; sx < n; sx++) {
            const uint32_t dx = mir ? (uint32_t)(INVADERS_GAME_HEIGHT - 1) - sx : sx;
            const uint8_t *vr = vram + dx * 32u;
            uint32_t lit = 0;
            for (uint32_t k = 0; k < nrow; k++) {
                const uint32_t c = 255u - (r0 + k);
                lit |= (uint32_t)(vr[c >> 3u] >> (c & 7u)) & 1u;
            }
            if (lit) buf[x] = COLOR_WHITE;
            x += rep[sx];
        }
        break;
    }

    case 1: {
        // 90 deg CCW (tate): canvas y -> dx, canvas x -> dy REVERSED.
        // VRAM's bit index is 255-dy (see the layout note above), so the
        // loop variable `col` IS 255-dy: x = x0 is dy 255, the player end.
        if (dvi_y < av_tate.y0 || dvi_y >= av_tate.y1) return;
        uint32_t dx = av_tate.row[dvi_y];
        if (mir) dx = (uint32_t)(INVADERS_GAME_HEIGHT - 1) - dx;
        const uint8_t *row = vram + dx * 32u;
        for (uint32_t x = av_tate.x0; x < av_tate.x1; x++) {
            const uint32_t col = av_tate.col[x];
            if ((row[col >> 3u] >> (col & 7u)) & 1u) buf[x] = COLOR_WHITE;
        }
        break;
    }

    case 2: {
        // 180 deg (landscape upside-down): dx reversed, dy un-reversed.
        // Merged, exactly as case 0 -- see the comment there. Only `col`'s
        // reversal and dx's ternary differ, which is what makes this 180
        // rather than a mirror of it (DEVNOTES #21).
        const uint32_t r0   = av_yoko.row[dvi_y];
        const uint32_t nrow = av_yoko.rowrep[dvi_y] ? av_yoko.rowrep[dvi_y] : 1u;
        const uint32_t n    = av_yoko.src_n;
        const uint8_t *rep  = av_yoko.rep;
        uint32_t x = av_yoko.x0;
        for (uint32_t sx = 0; sx < n; sx++) {
            const uint32_t dx = mir ? sx : (uint32_t)(INVADERS_GAME_HEIGHT - 1) - sx;
            const uint8_t *vr = vram + dx * 32u;
            uint32_t lit = 0;
            for (uint32_t k = 0; k < nrow; k++) {
                const uint32_t c = r0 + k;
                lit |= (uint32_t)(vr[c >> 3u] >> (c & 7u)) & 1u;
            }
            if (lit) buf[x] = COLOR_WHITE;
            x += rep[sx];
        }
        break;
    }

    case 3: {
        // 90 deg CW (tate): both axes reversed relative to case 1, so
        // x = x0 is dy 0 -- the score end.
        if (dvi_y < av_tate.y0 || dvi_y >= av_tate.y1) return;
        const uint32_t d  = av_tate.row[dvi_y];
        const uint32_t dx = mir ? d : (uint32_t)(INVADERS_GAME_HEIGHT - 1) - d;
        const uint8_t *row = vram + dx * 32u;
        for (uint32_t x = av_tate.x0; x < av_tate.x1; x++) {
            const uint32_t rev = (uint32_t)(INVADERS_GAME_WIDTH - 1u) - av_tate.col[x];
            if ((row[rev >> 3u] >> (rev & 7u)) & 1u) buf[x] = COLOR_WHITE;
        }
        break;
    }

    default: break;
    }
}

void invaders_video_render_scanline(uint32_t dvi_y, uint16_t *buf, const arcade_system *system) {
    render_scanline(dvi_y, buf, system);
}

void invaders_draw_frame(arcade_system *system) {
    // One submission per canvas row, in canvas coordinates -- there is no
    // longer a submission-index-to-physical-row conversion to get wrong.
    for (uint32_t i = 0; i < HAL_VIDEO_HEIGHT; i++) {
        uint16_t *buf = hal_video_acquire_scanline();
        render_scanline(i, buf, system);
        hal_video_submit_scanline(buf);
    }
}

void invaders_draw_error_frame(uint16_t color) {
    for (uint32_t i = 0; i < HAL_VIDEO_HEIGHT; i++) {
        uint16_t *buf = hal_video_acquire_scanline();
        for (uint32_t x = 0; x < HAL_VIDEO_WIDTH; x++) buf[x] = color;
        hal_video_submit_scanline(buf);
    }
}
