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
#include <Arduino.h> // micros() for the render-vs-block split in lrescue_draw_frame() below

// See invaders_video.cpp for the derivation of these constants -- identical
// video RAM layout and target monitor, so identical border/scale math.
#define TATE_BY  16u    // (480 - 224*2) / 2
#define TATE_BX  32u    // (320 - 256)   / 2
#define LAND_BX  48u    // (320 - 224)   / 2 -- centred in visible DVI x 0..319

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
    bool mir = sys->mirror_x;

    switch (sys->rotation) {

    case 0: {
        // Landscape: dx->DVI x x1, dy stretched x1.875 to fill 480 scanlines.
        uint32_t dy  = ((uint32_t)dvi_y * (uint32_t)LRESCUE_GAME_WIDTH) / HAL_VIDEO_HEIGHT;
        uint32_t col = 255u - dy;
        uint32_t chi = col >> 3u;
        for (uint32_t i = 0; i < (uint32_t)LRESCUE_GAME_HEIGHT; i++) {
            uint32_t dx = mir ? (uint32_t)(LRESCUE_GAME_HEIGHT - 1) - i : i;
            uint8_t v = vram[dx * 32u + chi];
            if ((v >> (col & 7u)) & 1u)
                buf[LAND_BX + i] = block_color(sys, dx, chi);
        }
        break;
    }

    case 1: {
        // 90 deg CCW (tate): DVI y->dx(x2), DVI x->dy reversed(x1, x=BX->dy255/player).
        if (dvi_y < TATE_BY || dvi_y >= TATE_BY + (uint32_t)LRESCUE_GAME_HEIGHT * 2u) return;
        uint32_t dx = (dvi_y - TATE_BY) >> 1u;
        if (mir) dx = (uint32_t)(LRESCUE_GAME_HEIGHT - 1) - dx;
        // Grouped by VRAM byte (chi) instead of iterating columns one at a
        // time: all 8 columns sharing one byte also share the same
        // block_color() result (its address only depends on dx>>3 and chi,
        // neither of which vary within a group) -- the original per-column
        // loop was re-reading the same byte up to 8x and recomputing/re-
        // looking-up the same color once per SET PIXEL instead of once per
        // 8-pixel group. Reading the byte once and skipping the whole group
        // outright when it's 0 (blank -- buf's already memset to black, so
        // there's nothing to write) cuts both to 1/8th their prior rate for
        // the common case of a mostly-empty playfield. bb (0..7) maps
        // directly to col&7 here since chi*8 is a multiple of 8 -- see
        // case 3 below for why that mapping isn't as simple when the column
        // order is reversed.
        const uint8_t *row = vram + dx * 32u;
        uint16_t *out = buf + TATE_BX;
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
        break;
    }

    case 2: {
        // 180 deg (landscape upside-down): dx reversed, dy reversed x1.875.
        uint32_t col = ((uint32_t)dvi_y * (uint32_t)LRESCUE_GAME_WIDTH) / HAL_VIDEO_HEIGHT; // reversed: top->dy255
        uint32_t chi = col >> 3u;
        for (uint32_t i = 0; i < (uint32_t)LRESCUE_GAME_HEIGHT; i++) {
            uint32_t dx = mir ? i : (uint32_t)(LRESCUE_GAME_HEIGHT - 1) - i;
            uint8_t v = vram[dx * 32u + chi];
            if ((v >> (col & 7u)) & 1u)
                buf[LAND_BX + i] = block_color(sys, dx, chi);
        }
        break;
    }

    case 3: {
        // 90 deg CW (tate): DVI y reversed->dx(x2), DVI x->dy(x1, x=BX->dy0/score).
        if (dvi_y < TATE_BY || dvi_y >= TATE_BY + (uint32_t)LRESCUE_GAME_HEIGHT * 2u) return;
        uint32_t dx = mir ? (dvi_y - TATE_BY) >> 1u
                          : (uint32_t)(LRESCUE_GAME_HEIGHT - 1) - ((dvi_y - TATE_BY) >> 1u);
        // Same grouped-by-byte approach as case 1 above, mirrored for this
        // rotation's reversed column order. LRESCUE_GAME_WIDTH is a
        // multiple of 8, so WIDTH-1-col0 always ends in binary ...111 (low
        // 3 bits = 7) whenever col0 itself is a multiple of 8 -- meaning
        // within one 8-column group, the bit tested counts down 7,6,...,0
        // as bb goes 0..7 (i.e. bit = 7-bb), never needing a borrow from
        // higher bits. That's what lets rev0/chi be computed once per
        // group instead of once per column, exactly like case 1.
        const uint8_t *row = vram + dx * 32u;
        uint16_t *out = buf + TATE_BX;
        for (uint32_t col0 = 0u; col0 < (uint32_t)LRESCUE_GAME_WIDTH; col0 += 8u) {
            uint32_t rev0 = (uint32_t)(LRESCUE_GAME_WIDTH - 1u) - col0;
            uint32_t chi = rev0 >> 3u;
            uint8_t v = row[chi];
            if (v) {
                uint16_t color = block_color(sys, dx, chi);
                for (uint32_t bb = 0u; bb < 8u; bb++) {
                    if ((v >> (7u - bb)) & 1u) out[bb] = color;
                }
            }
            out += 8u;
        }
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
    uint32_t step = HAL_VIDEO_HEIGHT / HAL_VIDEO_SCANLINES_PER_FRAME;
    uint32_t render_sum = 0, block_sum = 0;
    for (uint32_t i = 0; i < HAL_VIDEO_SCANLINES_PER_FRAME; i++) {
        uint32_t a = micros();
        uint16_t *buf = hal_video_acquire_scanline();
        uint32_t b = micros();
        render_scanline(i * step, buf, system);
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
    for (uint32_t i = 0; i < HAL_VIDEO_SCANLINES_PER_FRAME; i++) {
        uint16_t *buf = hal_video_acquire_scanline();
        for (uint32_t x = 0; x < HAL_VIDEO_WIDTH; x++) buf[x] = color;
        hal_video_submit_scanline(buf);
    }
}
