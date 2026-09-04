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
        const uint32_t col = 255u - av_yoko.row[dvi_y];
        const uint8_t  chi = (uint8_t)(col >> 3u);
        const uint8_t  bit = (uint8_t)(col & 7u);
        for (uint32_t x = av_yoko.x0; x < av_yoko.x1; x++) {
            const uint32_t i  = av_yoko.col[x];
            const uint32_t dx = mir ? (uint32_t)(INVADERS_GAME_HEIGHT - 1) - i : i;
            if ((vram[dx * 32u + chi] >> bit) & 1u) buf[x] = COLOR_WHITE;
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
        const uint32_t col = av_yoko.row[dvi_y];
        const uint8_t  chi = (uint8_t)(col >> 3u);
        const uint8_t  bit = (uint8_t)(col & 7u);
        for (uint32_t x = av_yoko.x0; x < av_yoko.x1; x++) {
            const uint32_t i  = av_yoko.col[x];
            const uint32_t dx = mir ? i : (uint32_t)(INVADERS_GAME_HEIGHT - 1) - i;
            if ((vram[dx * 32u + chi] >> bit) & 1u) buf[x] = COLOR_WHITE;
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
