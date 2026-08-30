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

// Border/scale constants, calibrated for a 640x480 4:3 monitor -- see this
// file's header comment and invaders_pico's DEVNOTES.md for the derivation.
// Tate (90/270 deg): DVI x visible range is 0..319 (physical monitor width after rotation).
//   Game columns (224) along DVI y x2 = 448 px; game rows (256) along DVI x x1 = 256 px.
// Landscape (0/180 deg): DVI x visible range is still 0..319 (physical monitor height).
//   DVI x pixels are 2x the physical size of DVI y pixels on this 4:3 monitor at this
//   resolution, so game columns (224) go along DVI x x1, game rows (256) are stretched
//   x1.875 into DVI y (dy = dvi_y * 256 / 480) to compensate -- fills all 480 scanlines,
//   no crop, near-square pixels.
#define TATE_BY  16u    // (480 - 224*2) / 2
#define TATE_BX  32u    // (320 - 256)   / 2
#define LAND_BX  48u    // (320 - 224)   / 2 -- centred in visible DVI x 0..319

#define COLOR_WHITE 0xFFFFu

// VRAM layout: column-major, dx=0..223 left->right, dy=0..255 top(score)->bottom(player).
//   byte = memory[0x2400 + dx*32 + (255-dy)/8],  bit = (255-dy)%8

static void render_scanline(uint32_t dvi_y, uint16_t *buf, const arcade_system *sys) {
    memset(buf, 0, HAL_VIDEO_WIDTH * sizeof(uint16_t));
    const uint8_t *vram = sys->state.memory + 0x2400;
    bool mir = sys->mirror_x;

    switch (sys->rotation) {

    case 0: {
        // Landscape: dx->DVI x x1, dy stretched x1.875 to fill 480 scanlines.
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
        // 90 deg CCW (tate): DVI y->dx(x2), DVI x->dy reversed(x1, x=BX->dy255/player).
        if (dvi_y < TATE_BY || dvi_y >= TATE_BY + (uint32_t)INVADERS_GAME_HEIGHT * 2u) return;
        uint32_t dx = (dvi_y - TATE_BY) >> 1u;
        if (mir) dx = (uint32_t)(INVADERS_GAME_HEIGHT - 1) - dx;
        for (uint32_t col = 0u; col < (uint32_t)INVADERS_GAME_WIDTH; col++) {
            uint8_t v = vram[dx * 32u + (col >> 3u)];
            if ((v >> (col & 7u)) & 1u)
                buf[TATE_BX + col] = COLOR_WHITE;
        }
        break;
    }

    case 2: {
        // 180 deg (landscape upside-down): dx reversed, dy reversed x1.875.
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
        // 90 deg CW (tate): DVI y reversed->dx(x2), DVI x->dy(x1, x=BX->dy0/score).
        if (dvi_y < TATE_BY || dvi_y >= TATE_BY + (uint32_t)INVADERS_GAME_HEIGHT * 2u) return;
        uint32_t dx = mir ? (dvi_y - TATE_BY) >> 1u
                          : (uint32_t)(INVADERS_GAME_HEIGHT - 1) - ((dvi_y - TATE_BY) >> 1u);
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
    // HAL_VIDEO_SCANLINES_PER_FRAME may be less than HAL_VIDEO_HEIGHT (see
    // arcade_hal_video.h) -- `step` maps each submission index back onto
    // the physical-row coordinate space the rotation/mirror math above is
    // calibrated against.
    uint32_t step = HAL_VIDEO_HEIGHT / HAL_VIDEO_SCANLINES_PER_FRAME;
    for (uint32_t i = 0; i < HAL_VIDEO_SCANLINES_PER_FRAME; i++) {
        uint16_t *buf = hal_video_acquire_scanline();
        render_scanline(i * step, buf, system);
        hal_video_submit_scanline(buf);
    }
}

void invaders_draw_error_frame(uint16_t color) {
    for (uint32_t i = 0; i < HAL_VIDEO_SCANLINES_PER_FRAME; i++) {
        uint16_t *buf = hal_video_acquire_scanline();
        for (uint32_t x = 0; x < HAL_VIDEO_WIDTH; x++) buf[x] = color;
        hal_video_submit_scanline(buf);
    }
}
