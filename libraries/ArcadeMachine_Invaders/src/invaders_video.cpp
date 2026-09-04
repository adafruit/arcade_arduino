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

// Border constants, in CANVAS coordinates: 320x240 logical square pixels,
// each displayed as a 2x2 block of physical pixels (see arcade_hal_video.h).
// Tate (90/270 deg): the monitor is physically rotated, so canvas x is the
//   screen's long axis and canvas y its short one. Game rows (256) run along
//   canvas x; game columns (224) run along canvas y.
// Landscape (0/180 deg): game columns (224) run along canvas x; game rows
//   (256) are resampled onto the 240 canvas rows by
//   `dy = dvi_y * 256 / HAL_VIDEO_HEIGHT`.
//
// THESE ARE POSITIONS, NOT RATIOS, and the difference is load-bearing: the
// vertical one halved when the canvas stopped being declared as the physical
// 480 rows, while the landscape divisor above did not change meaning at all
// because it is written against HAL_VIDEO_HEIGHT. Getting that backwards is
// DEVNOTES #23/#76.
//
// NOTE these do NOT make the picture match the original cabinet's aspect
// ratio -- nothing here scales to the screen, so tate is 16.7% too wide for
// its height and landscape 24.4%. That is a known, separate defect; see
// DISPLAY_GEOMETRY.md section 2 and its phase 2.
#define TATE_BY   8u    // (240 - 224) / 2
#define TATE_BX  32u    // (320 - 256) / 2
#define LAND_BX  48u    // (320 - 224) / 2

#define COLOR_WHITE 0xFFFFu

// VRAM layout: column-major, dx=0..223 left->right, dy=0..255 top(score)->bottom(player).
//   byte = memory[0x2400 + dx*32 + (255-dy)/8],  bit = (255-dy)%8

static void render_scanline(uint32_t dvi_y, uint16_t *buf, const arcade_system *sys) {
    memset(buf, 0, HAL_VIDEO_WIDTH * sizeof(uint16_t));
    const uint8_t *vram = sys->state.memory + 0x2400;
    bool mir = sys->mirror_x;

    switch (sys->rotation) {

    case 0: {
        // Landscape: dx -> canvas x 1:1, dy resampled onto all 240 canvas rows.
        uint32_t dy  = ((uint32_t)dvi_y * (uint32_t)INVADERS_GAME_WIDTH) / HAL_VIDEO_HEIGHT;
        uint32_t col = 255u - dy;
        for (uint32_t i = 0; i < (uint32_t)INVADERS_GAME_HEIGHT; i++) {
            uint32_t dx = mir ? (uint32_t)(INVADERS_GAME_HEIGHT - 1) - i : i;
            uint8_t v = vram[dx * 32u + (col >> 3u)];
            if ((v >> (col & 7u)) & 1u)
                buf[LAND_BX + i] = COLOR_WHITE;
        }
        break;
    }

    case 1: {
        // 90 deg CCW (tate): canvas y->dx (1:1), canvas x->dy reversed
        // (x=BX -> dy 255, the player end).
        if (dvi_y < TATE_BY || dvi_y >= TATE_BY + (uint32_t)INVADERS_GAME_HEIGHT) return;
        uint32_t dx = dvi_y - TATE_BY;
        if (mir) dx = (uint32_t)(INVADERS_GAME_HEIGHT - 1) - dx;
        for (uint32_t col = 0u; col < (uint32_t)INVADERS_GAME_WIDTH; col++) {
            uint8_t v = vram[dx * 32u + (col >> 3u)];
            if ((v >> (col & 7u)) & 1u)
                buf[TATE_BX + col] = COLOR_WHITE;
        }
        break;
    }

    case 2: {
        // 180 deg (landscape upside-down): dx reversed, dy un-reversed.
        uint32_t col = ((uint32_t)dvi_y * (uint32_t)INVADERS_GAME_WIDTH) / HAL_VIDEO_HEIGHT; // reversed: top->dy255
        for (uint32_t i = 0; i < (uint32_t)INVADERS_GAME_HEIGHT; i++) {
            uint32_t dx = mir ? i : (uint32_t)(INVADERS_GAME_HEIGHT - 1) - i;
            uint8_t v = vram[dx * 32u + (col >> 3u)];
            if ((v >> (col & 7u)) & 1u)
                buf[LAND_BX + i] = COLOR_WHITE;
        }
        break;
    }

    case 3: {
        // 90 deg CW (tate): canvas y reversed->dx (1:1), canvas x->dy
        // (x=BX -> dy 0, the score end).
        if (dvi_y < TATE_BY || dvi_y >= TATE_BY + (uint32_t)INVADERS_GAME_HEIGHT) return;
        uint32_t dx = mir ? (dvi_y - TATE_BY)
                          : (uint32_t)(INVADERS_GAME_HEIGHT - 1) - (dvi_y - TATE_BY);
        for (uint32_t col = 0u; col < (uint32_t)INVADERS_GAME_WIDTH; col++) {
            uint32_t rev = (uint32_t)(INVADERS_GAME_WIDTH - 1u) - col; // dy=0 at x=TATE_BX
            uint8_t v = vram[dx * 32u + (rev >> 3u)];
            if ((v >> (rev & 7u)) & 1u)
                buf[TATE_BX + col] = COLOR_WHITE;
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
