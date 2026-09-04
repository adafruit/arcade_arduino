// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Lunar Rescue VRAM renderer. Rotation/mirror/stretch math is copied
// unchanged from ArcadeMachine_Invaders' invaders_video.cpp (identical VRAM
// layout and 640x480 4:3 target monitor); the only real difference is
// per-block color instead of a fixed white foreground -- see
// block_color() below.
//
// block_color()'s address formula was derived from, and numerically
// verified byte-for-byte against, MAME's screen_update_invadpt2()
// (midw8080/8080bw_v.cpp):
//   offs_t color_address = (offs >> 8 << 5) | (offs & 0x1f);
//   uint8_t fore_color = m_screen_red ? 1 : color_map_base[color_address] & 0x07;
// where `offs` there is a byte offset into the FULL 0x2000-byte main_ram
// share (0x2000-0x3fff), whereas this file's `dx`/`col>>3` are relative to
// vram = memory+0x2400 (i.e. offs - 0x400) -- see lrescue_ports.cpp's
// sibling comment blocks for why that base is correct. Working through the
// algebra (offs = voffs + 0x400, and 0x400 is a multiple of 32 so it drops
// out of the low 5 bits) collapses to:
//   color_address = 128 + 32*(dx >> 3) + (col >> 3)
// A 7168-entry brute-force check against MAME's original formula (every
// visible VRAM byte) confirmed zero mismatches before this was written.
//
// The color-index -> RGB565 mapping is transcribed from MAME's
// palette_init_3bit_rbg() (emu/emupal.cpp):
//   rgb_t(pal1bit(i>>0), pal1bit(i>>2), pal1bit(i>>1))  // (R, G, B) from bits (0, 2, 1) of i
#include <string.h>
#include "lrescue_video.h"
#include "arcade_hal_video.h"
#include "arcade_video_geom.h"
#include <Arduino.h> // micros() for the render-vs-block split in lrescue_draw_frame() below

// Where the picture lands on the canvas comes from ArcadeHAL's
// arcade_video_geom.h -- av_tate/av_yoko, built by av_geom_init() in
// lrescue_init(). The TATE_BX/TATE_BY/LAND_BX constants this file used to
// copy from invaders_video.cpp are gone; that copying is exactly what
// DEVNOTES #21/#23/#33 came from.

// Index i's bits map to (R=bit0, G=bit2, B=bit1) per palette_init_3bit_rbg()
// above -- NOT the more intuitive bit0/1/2=R/G/B order the "RBG" name hints
// at but doesn't spell out.
static const uint16_t LRESCUE_PALETTE[8] = {
    0x0000, // 0: black
    0xF800, // 1: red    (also the fixed "screen red" fore-color)
    0x001F, // 2: blue
    0xF81F, // 3: magenta
    0x07E0, // 4: green
    0xFFE0, // 5: yellow
    0x07FF, // 6: cyan
    0xFFFF, // 7: white
};

// col_hi3 = col>>3 (0..31). See file header comment for the derivation.
static inline uint16_t block_color(const arcade_system *sys, uint32_t dx, uint32_t col_hi3) {
    if (sys->screen_red) return LRESCUE_PALETTE[1];
    uint32_t addr = 128u + 32u * (dx >> 3u) + col_hi3;
    uint8_t idx = sys->color_prom[addr] & 0x07u;
    return LRESCUE_PALETTE[idx];
}

// VRAM layout: identical to Space Invaders' -- column-major, dx=0..223
// left->right, dy=0..255 top(score)->bottom(player).
//   byte = memory[0x2400 + dx*32 + (255-dy)/8],  bit = (255-dy)%8

static void render_scanline(uint32_t dvi_y, uint16_t *buf, const arcade_system *sys) {
    memset(buf, 0, HAL_VIDEO_WIDTH * sizeof(uint16_t));
    const uint8_t *vram = sys->state.memory + 0x2400;
    // Scratch for the tate cases' byte-grouped decode -- see case 1.
    static uint16_t scratch[LRESCUE_GAME_WIDTH];
    bool mir = sys->mirror_x;

    switch (sys->rotation) {

    case 0: {
        // Landscape. av_yoko.row maps the canvas row onto the LONG axis;
        // av_yoko.col maps canvas columns onto the SHORT axis, which in yoko
        // must NARROW to 180 canvas columns rather than sit at 1:1. See
        // arcade_video_geom.h.
        const uint32_t col = 255u - av_yoko.row[dvi_y];
        const uint32_t chi = col >> 3u;
        const uint32_t bit = col & 7u;
        for (uint32_t x = av_yoko.x0; x < av_yoko.x1; x++) {
            const uint32_t i  = av_yoko.col[x];
            const uint32_t dx = mir ? (uint32_t)(LRESCUE_GAME_HEIGHT - 1) - i : i;
            if ((vram[dx * 32u + chi] >> bit) & 1u)
                buf[x] = block_color(sys, dx, chi);
        }
        break;
    }

    case 1: {
        // 90 deg CCW (tate): canvas y -> dx, canvas x -> dy reversed
        // (x = x0 is dy 255, the player end).
        if (dvi_y < av_tate.y0 || dvi_y >= av_tate.y1) return;
        uint32_t dx = av_tate.row[dvi_y];
        if (mir) dx = (uint32_t)(LRESCUE_GAME_HEIGHT - 1) - dx;

        // Decoded into a scratch row, then mapped onto the canvas through
        // av_tate.col. The scratch step is what PRESERVES the byte-grouped
        // decode below, which is a measured win worth keeping: all 8 columns
        // sharing one VRAM byte also share one block_color() result (its
        // address depends only on dx>>3 and chi, neither varying within a
        // group), so reading the byte once and skipping the whole group when
        // it is zero cuts both the reads and the colour lookups to an eighth
        // for the common mostly-empty playfield. That grouping needs 8
        // CONSECUTIVE destination slots, which the canvas no longer
        // guarantees once the column map resamples -- hence the scratch row
        // rather than writing straight into `buf`. bb (0..7) maps directly
        // to col&7 here since chi*8 is a multiple of 8; see case 3 for why
        // that is less simple when the column order is reversed.
        memset(scratch, 0, sizeof(uint16_t) * LRESCUE_GAME_WIDTH);
        const uint8_t *row = vram + dx * 32u;
        uint16_t *out = scratch;
        for (uint32_t chi = 0u; chi < (uint32_t)LRESCUE_GAME_WIDTH / 8u; chi++) {
            uint8_t v = row[chi];
            if (v) {
                uint16_t color = block_color(sys, dx, chi);
                for (uint32_t bb = 0u; bb < 8u; bb++) {
                    if ((v >> bb) & 1u) out[bb] = color;
                }
            }
            out += 8u;
        }
        for (uint32_t x = av_tate.x0; x < av_tate.x1; x++)
            buf[x] = scratch[av_tate.col[x]];
        break;
    }

    case 2: {
        // 180 deg (landscape upside-down): dx reversed, dy un-reversed.
        const uint32_t col = av_yoko.row[dvi_y];
        const uint32_t chi = col >> 3u;
        const uint32_t bit = col & 7u;
        for (uint32_t x = av_yoko.x0; x < av_yoko.x1; x++) {
            const uint32_t i  = av_yoko.col[x];
            const uint32_t dx = mir ? i : (uint32_t)(LRESCUE_GAME_HEIGHT - 1) - i;
            if ((vram[dx * 32u + chi] >> bit) & 1u)
                buf[x] = block_color(sys, dx, chi);
        }
        break;
    }

    case 3: {
        // 90 deg CW (tate): both axes reversed relative to case 1, so
        // x = x0 is dy 0, the score end.
        if (dvi_y < av_tate.y0 || dvi_y >= av_tate.y1) return;
        const uint32_t d  = av_tate.row[dvi_y];
        const uint32_t dx = mir ? d : (uint32_t)(LRESCUE_GAME_HEIGHT - 1) - d;

        // Same grouped-by-byte decode as case 1, into the same scratch row;
        // the reversal is applied when mapping onto the canvas below rather
        // than inside the decode, which keeps the grouping intact.
        memset(scratch, 0, sizeof(uint16_t) * LRESCUE_GAME_WIDTH);
        const uint8_t *row = vram + dx * 32u;
        uint16_t *out = scratch;
        for (uint32_t chi = 0u; chi < (uint32_t)LRESCUE_GAME_WIDTH / 8u; chi++) {
            uint8_t v = row[chi];
            if (v) {
                uint16_t color = block_color(sys, dx, chi);
                for (uint32_t bb = 0u; bb < 8u; bb++) {
                    if ((v >> bb) & 1u) out[bb] = color;
                }
            }
            out += 8u;
        }
        for (uint32_t x = av_tate.x0; x < av_tate.x1; x++)
            buf[x] = scratch[(uint32_t)(LRESCUE_GAME_WIDTH - 1u) - av_tate.col[x]];
        break;
    }

    default: break;
    }
}

// Last frame's totals, split into pure render *compute* time vs. time
// blocked inside hal_video_acquire_scanline()/hal_video_submit_scanline()
// waiting on Core 1 -- see lrescue_draw_frame()'s doc comment for why that
// split matters. Exposed via lrescue_video_debug_last_frame_us() so
// lrescue_run_frame() can fold them into its own once-per-second report
// without this file needing to know anything about that report's cadence
// or format.
static uint32_t g_last_render_us = 0, g_last_block_us = 0;

void lrescue_video_debug_last_frame_us(uint32_t *render_us, uint32_t *block_us) {
    if (render_us) *render_us = g_last_render_us;
    if (block_us)  *block_us  = g_last_block_us;
}

void lrescue_video_render_scanline(uint32_t dvi_y, uint16_t *buf, const arcade_system *system) {
    render_scanline(dvi_y, buf, system);
}

void lrescue_draw_frame(arcade_system *system) {
    // hal_video_acquire_scanline()/hal_video_submit_scanline() deliberately
    // BLOCK to pace Core 0 against Core 1's real DVI rate -- that's this
    // project's documented "natural ~60Hz limiter" (no explicit frame
    // timer exists anywhere). That means a plain "how long did the whole
    // frame take" measurement (as lrescue_run_frame() briefly used) will
    // read close to the true frame period basically ALWAYS, regardless of
    // whether Core 0 is actually falling behind or comfortably idle-
    // waiting -- the two look identical in that one number. Splitting
    // render-compute (this file's own CPU-bound work: color-PROM lookups,
    // writes into buf[]) from block-wait (time spent inside those two
    // calls specifically) is what actually distinguishes them. Summed
    // across the whole frame here (not per-scanline-printed, unlike an
    // earlier version of this instrumentation) to keep overhead to just
    // cheap micros() reads, no Serial calls, in this loop.
    uint32_t render_sum = 0, block_sum = 0;
    for (uint32_t i = 0; i < HAL_VIDEO_HEIGHT; i++) {
        uint32_t a = micros();
        uint16_t *buf = hal_video_acquire_scanline();
        uint32_t b = micros();
        render_scanline(i, buf, system);
        uint32_t c = micros();
        hal_video_submit_scanline(buf);
        uint32_t d = micros();
        block_sum  += (b - a) + (d - c);
        render_sum += (c - b);
    }
    g_last_render_us = render_sum;
    g_last_block_us  = block_sum;
}

void lrescue_draw_error_frame(uint16_t color) {
    for (uint32_t i = 0; i < HAL_VIDEO_HEIGHT; i++) {
        uint16_t *buf = hal_video_acquire_scanline();
        for (uint32_t x = 0; x < HAL_VIDEO_WIDTH; x++) buf[x] = color;
        hal_video_submit_scanline(buf);
    }
}
