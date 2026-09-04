// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Galaga tile+sprite renderer -- see galaga_video.h for scope and the
// citation trail. Every formula below is cited against the exact MAME
// source line(s) it came from (src/mame/namco/galaga_v.cpp, fetched and
// read directly this session, not recalled).
#include <string.h>
#include "galaga_video.h"
#include "arcade_hal_video.h"
#include "arcade_video_geom.h"

uint8_t galaga_gfx1_rom[GALAGA_GFX1_SIZE];
uint8_t galaga_gfx2_rom[GALAGA_GFX2_SIZE];
uint8_t galaga_palette_prom[GALAGA_PALETTE_PROM_SIZE];
uint8_t galaga_char_lookup_prom[GALAGA_CHAR_LOOKUP_SIZE];
uint8_t galaga_sprite_lookup_prom[GALAGA_SPRITE_LOOKUP_SIZE];

// Hot rendering path -> SRAM instead of flash, same XIP-cache rationale as
// ArcadeCPU_Z80's Z80_RAMFUNC (see z80.c for the full explanation).
#if defined(ARDUINO_ARCH_RP2040) || defined(PICO_ON_DEVICE)
#define GALAGA_VID_RAMFUNC __attribute__((section(".time_critical.galagavid")))
#else
#define GALAGA_VID_RAMFUNC
#endif

#define NUM_TILES   256
#define NUM_SPRITES 128

// Decoded caches, built once by galaga_video_build_caches().
static uint8_t tile_pixels[NUM_TILES][8][8];      // [tile][x][y] -> 2bpp pixel (0-3)
static uint8_t sprite_pixels[NUM_SPRITES][16][16]; // [sprite][x][y] -> 2bpp pixel (0-3)
static uint16_t rgb565_palette[32];

// Flattened pen lookups: final RGB565 for every (color code, 2bpp pixel)
// pair, built once. The per-pixel path used to do two dependent lookups
// (colour PROM, then core palette) plus masking for EVERY pixel drawn --
// ~64k tile pixels per frame before sprites. Precomputing 64x4 entries
// (512 bytes each) turns that into one indexed load. Same values, just
// hoisted; char and sprite stay separate because they draw from different
// halves of the core palette (see char_pen_color/sprite_pen_color).
static uint16_t char_pen_rgb[64][4];
static uint16_t sprite_pen_rgb[64][4];

// Border/scale constants for the 640x480 DVI output -- IDENTICAL to
// pacman_video.cpp's, because GALAGA_GAME_WIDTH/HEIGHT (288x224) are
// byte-for-byte the same as PACMAN_GAME_WIDTH/HEIGHT (same Namco video-
// generator family/raster timing, verified in galaga_machine.h's header
// comment) -- not re-derived, just reused.

// Maps logical tilemap (col 0-35, row 0-27) to its video_ram byte offset
// within the tile-code half (0x000-0x3FF). Verified against MAME's
// galaga_state::tilemap_scan() (galaga_v.cpp) -- identical formula to
// pacman_video.cpp's scan_rows(), same Namco tilegen IC.
static inline uint16_t scan_rows(uint32_t col, uint32_t row) {
    row = (row + 2) & 0xFFu;
    col = (col - 2) & 0xFFu;
    if (col & 0x20u)
        return (uint16_t)(row + ((col & 0x1Fu) << 5));
    else
        return (uint16_t)(col + (row << 5));
}

// Bit-address-based gfx decode -- deliberately NOT a hand-derived
// byte-index shortcut (the way pacman_video.cpp's pixel2_from_byte() is):
// Galaga's spritelayout_galaga has a DIFFERENT xoffset arrangement than
// Pac-Man's spritelayout_bosco-derived one (the four STEP4() x-groups are
// in a different order), so a shortcut ported from Pac-Man's code would
// silently scramble sprites -- exactly the "Pac-Man looks like a tennis
// ball" class of bug pacman_video.cpp's own header comment warns about.
// This computes each pixel directly from MAME's own gfx_layout tables
// (planeoffset={0,4}, xoffset/yoffset below, both quoted verbatim from
// charlayout_2bpp/spritelayout_galaga in galaga.cpp), so it's directly
// auditable against the source rather than hoped-equivalent.
// readbit() convention (MSB-first) verified against drawgfx.cpp's
// gfx_element::decode(), same fact pacman_video.cpp's header comment
// already documents (universal to MAME's decoder, not layout-specific).
static inline int readbit(const uint8_t *src, int bitaddr) {
    return (src[bitaddr >> 3] >> (7 - (bitaddr & 7))) & 1;
}
static inline uint8_t decode_pixel(const uint8_t *base, int xoff, int yoff) {
    // Plane 0 -> high bit of pixel value, plane 1 -> low bit (fixed
    // drawgfx.cpp convention, independent of planeoffset's actual values).
    int p0 = readbit(base, 0 + xoff + yoff); // planeoffset[0] = 0
    int p1 = readbit(base, 4 + xoff + yoff); // planeoffset[1] = 4
    return (uint8_t)((p0 << 1) | p1);
}

// charlayout_2bpp: 8x8, xoffset={STEP4(8*8,1),STEP4(0*8,1)}, yoffset={STEP8(0*8,8)}.
static const int TILE_XOFFSET[8] = { 64, 65, 66, 67, 0, 1, 2, 3 };
static const int TILE_YOFFSET[8] = { 0, 8, 16, 24, 32, 40, 48, 56 };

// spritelayout_galaga: 16x16, xoffset={STEP4(0*8,1),STEP4(8*8,1),STEP4(16*8,1),STEP4(24*8,1)},
// yoffset={STEP8(0*8,8),STEP8(32*8,8)}.
static const int SPRITE_XOFFSET[16] = {
    0, 1, 2, 3, 64, 65, 66, 67, 128, 129, 130, 131, 192, 193, 194, 195
};
static const int SPRITE_YOFFSET[16] = {
    0, 8, 16, 24, 32, 40, 48, 56, 256, 264, 272, 280, 288, 296, 304, 312
};

// Forward declarations: build_caches() fills the flattened pen LUTs from
// these, and they are defined below next to the palette commentary.
// star_build_tables() likewise lives with the rest of the 05XX starfield
// code further down.
static void star_build_tables(void);
static inline uint16_t char_pen_color(uint8_t color6, uint8_t pixel2);
static inline uint16_t sprite_pen_color(uint8_t color6, uint8_t pixel2);

void galaga_video_build_caches(void) {
    for (int t = 0; t < NUM_TILES; t++) {
        const uint8_t *base = &galaga_gfx1_rom[t * 16]; // charincrement = 16*8 bits = 16 bytes
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                tile_pixels[t][x][y] = decode_pixel(base, TILE_XOFFSET[x], TILE_YOFFSET[y]);
    }

    for (int s = 0; s < NUM_SPRITES; s++) {
        const uint8_t *base = &galaga_gfx2_rom[s * 64]; // charincrement = 64*8 bits = 64 bytes
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
                sprite_pixels[s][x][y] = decode_pixel(base, SPRITE_XOFFSET[x], SPRITE_YOFFSET[y]);
    }

    // Core 32-entry palette, resistor-ladder DAC -- verified against
    // galaga_v.cpp's galaga_palette(): resistances {1000,470,220} ohm for
    // R/G, {470,220} ohm for B -- the EXACT same values (and therefore
    // the exact same derived weights) pacman_video.cpp already computes
    // for its own 82s123.7f palette PROM, reused unchanged rather than
    // recomputed.
    static const double RGWEIGHT[3] = { 33.232922, 70.708344, 151.058735 }; // 1000/470/220 ohm
    static const double BWEIGHT[2]  = { 81.304348, 173.695652 };           // 470/220 ohm
    for (int i = 0; i < 32; i++) {
        uint8_t b = galaga_palette_prom[i];
        int r = (int)(RGWEIGHT[0] * ((b >> 0) & 1) + RGWEIGHT[1] * ((b >> 1) & 1) + RGWEIGHT[2] * ((b >> 2) & 1) + 0.5);
        int g = (int)(RGWEIGHT[0] * ((b >> 3) & 1) + RGWEIGHT[1] * ((b >> 4) & 1) + RGWEIGHT[2] * ((b >> 5) & 1) + 0.5);
        int bl = (int)(BWEIGHT[0] * ((b >> 6) & 1) + BWEIGHT[1] * ((b >> 7) & 1) + 0.5);
        rgb565_palette[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (bl >> 3));
    }

    for (int c = 0; c < 64; c++) {
        for (int p = 0; p < 4; p++) {
            char_pen_rgb[c][p]   = char_pen_color((uint8_t)c, (uint8_t)p);
            sprite_pen_rgb[c][p] = sprite_pen_color((uint8_t)c, (uint8_t)p);
        }
    }

    star_build_tables();
}

// Character pen -- verified against galaga_palette(): char lookup bytes
// index into indirect-color range 16-31 (`(*color_prom++ & 0x0f) | 0x10`),
// i.e. characters draw from the UPPER half of the 32-entry core palette.
static inline uint16_t char_pen_color(uint8_t color6, uint8_t pixel2) {
    uint8_t idx = (uint8_t)((color6 & 0x3Fu) * 4u + pixel2);
    uint8_t pen = (uint8_t)((galaga_char_lookup_prom[idx] & 0x0Fu) | 0x10u);
    return rgb565_palette[pen];
}
// Sprite pen -- sprite lookup bytes index into 0-15 (`*color_prom++ &
// 0x0f`, no |0x10), i.e. sprites draw from the LOWER half of the same
// palette -- characters and sprites deliberately do NOT share color
// range, a real and easy-to-miss detail of this hardware.
static inline uint16_t sprite_pen_color(uint8_t color6, uint8_t pixel2) {
    uint8_t idx = (uint8_t)((color6 & 0x3Fu) * 4u + pixel2);
    uint8_t pen = (uint8_t)(galaga_sprite_lookup_prom[idx] & 0x0Fu);
    return rgb565_palette[pen];
}

// gfx_offs quadrant-select table for multi-cell sprites -- verified
// verbatim against galaga_v.cpp's draw_sprites(): a sprite's `sizex`/
// `sizey` bits (0 or 1) mean it occupies (sizex+1) x (sizey+1) cells of
// 16x16 pixels each (up to 32x32 total), and which of 4 consecutive gfx2
// tile indices (code+0..code+3) fills which destination cell is looked
// up here, XORed with the flip state so a flipped multi-cell sprite's
// quadrants swap correctly, not just each quadrant's own pixels.
static const int GFX_OFFS[2][2] = { { 0, 1 }, { 2, 3 } };

// Renders one native row (0..GALAGA_GAME_HEIGHT-1) of starfield, sprites
// and tilemap into `out` (GALAGA_GAME_WIDTH RGB565 pixels). Draw order --
// background (black, then the 05XX starfield),
// sprites, THEN tilemap on top -- is verified against galaga_v.cpp's
// screen_update_galaga() and is deliberately the OPPOSITE of Pac-Man's
// tiles-then-sprites order; Galaga's tilemap carries the score/UI text
// overlay and must composite on top of everything. Real hardware uses a
// per-tile-color transparency GROUP (tilemap->configure_groups()) whose
// exact opaque/transparent pen assignment this session didn't fully
// trace -- this renderer approximates it by treating tile pixel value 0
// as transparent (letting sprites show through blank tile cells) and
// every other pixel value as opaque, which is the general 2bpp
// convention used everywhere else in this project's renderers; flag this
// specific approximation if real hardware shows tilemap/sprite
// compositing looking wrong.

// ---------------------------------------------------------------------------
// Namco 05XX starfield
//
// Verified against MAME's starfield_05xx.cpp, which is itself a reverse-
// engineering of a real 1981 Namco 05XX from a Galaga board (R. Hildinger,
// 2019) -- so the constants below are traced silicon behaviour, not guesses:
//
//  - 16-bit Fibonacci LFSR, taps at 16/13/11/6, maximal period 65535.
//  - It runs continuously over a 256x256 pixel field and is NOT reset per
//    frame. A frame consumes 65536 clocks against a 65535-long sequence,
//    which is exactly why the field drifts 1 pixel per frame on its own;
//    ALL scrolling is done by running extra clocks, or skipping them,
//    during blanking.
//  - A "hit" (a star) is (lfsr & 0xFA14) == 0x7800; the surviving bits then
//    give a 2-bit star-set and a 6-bit colour.
//  - 256 stars live in 4 sets of 64; exactly two sets are displayed at once.
//
// Galaga's wiring, from galaga_v.cpp: X speed = videolatch Q2..Q0, Y speed
// is hardwired 0 (Galaga never scrolls vertically), sets = Q3 and Q4|2,
// and Q5 is _STARCLR (enable). Config is set_starfield_config(16, 0, 272),
// i.e. x offset 16, y offset 0, and an x limit that never actually rejects
// anything at 256 pixels wide.
//
// IMPLEMENTATION NOTE -- this does NOT step the LFSR per pixel the way MAME
// does. That would be ~65536 LFSR steps every frame, which on this board is
// real money against a 16.67ms budget with only ~2.3ms spare. Since the
// sequence is fixed, the hit positions are precomputed ONCE into a table;
// each frame then only has to offset into that table (~256 entries) and
// bucket the visible stars by row. Same output, a few hundred operations
// per frame instead of tens of thousands.
#define STAR_LFSR_SEED   0x7FFFu
#define STAR_HIT_MASK    0xFA14u
#define STAR_HIT_VALUE   0x7800u
#define STAR_PERIOD      65535u   // maximal LFSR sequence length
#define STAR_LINE_CLOCKS 256u     // LFSR clocks per scanline
#define STAR_VIS_LINES   224u
#define STAR_VIS_CLOCKS  (STAR_VIS_LINES * STAR_LINE_CLOCKS)
#define STAR_PRE_VIS     (22u * STAR_LINE_CLOCKS) // speed_index_Y == 0 row of
#define STAR_POST_VIS    (10u * STAR_LINE_CLOCKS) // MAME's pre/post tables
#define STAR_OFFSET_X    16
// Both of these are EXACT bounds, not estimates with headroom. Derived by
// exhaustively enumerating the LFSR: walk all 65535 states from the seed
// (it returns to 0x7FFF on step 65535, confirming the taps give the maximal
// period), collect the hits, then for every one of the 4 set-pairs at every
// one of the 65535 possible frame phases, bucket the visible stars by row.
// Results, which also independently confirm the decode against MAME's prose:
//   - exactly 256 hits per period, split 64/64/64/64 across the 4 sets,
//     matching starfield_05xx.cpp's "256 stars in 4 sets of 64";
//   - never more than 5 stars land on the same row, in any configuration;
//   - 103..120 stars on screen per frame (mean 112 = 128 * 224/256, i.e.
//     two 64-star sets minus the fraction that falls in vertical blanking).
#define STAR_MAX_HITS    256
#define STAR_MAX_PER_ROW 5

// X scroll: extra (or skipped) LFSR clocks per frame, indexed by Q2..Q0.
// Verbatim from starfield_05xx.cpp's speed_X_cycle_count_offset[].
//
// Note the axis names are the 05XX's, not the screen's. This is the
// HARDWARE raster, which for Galaga is 288 wide x 224 tall and only becomes
// a portrait screen after the cabinet's ROT90 -- so the chip's "X" is the
// player's VERTICAL axis, which is why a vertical scroller only ever needed
// an X speed and has Y hardwired to 0.
//
// The SIGN here is the one thing a transcription can silently get backwards,
// so it was checked against the game rather than assumed. Net drift per
// frame is 1 + offset (the +1 being the free 65536-vs-65535 drift), which
// makes index 7 (offset -1) an exact standstill and index 6 one pixel of
// travel. Dumping this latch every frame through the host harness shows
// Galaga writes ONLY those two values -- speed 6 while the field scrolls,
// speed 7 while it is frozen for the stage-intro sequences. A game having a
// "stars stopped" state that lands on precisely zero net drift is not a
// coincidence the opposite sign convention can produce: flipping it turns
// index 7 into 2px/frame and leaves Galaga with no way to stop the stars.
static const int8_t STAR_SPEED_X[8] = { 0, 1, 2, 3, -4, -3, -2, -1 };

typedef struct { uint16_t pos; uint8_t set; uint8_t color; } star_hit_t;
static star_hit_t star_hits[STAR_MAX_HITS];
static int        star_hit_count = 0;
static uint16_t   star_palette[64];

// Sequence position at the START of the current frame. Free-running, like
// the real chip -- only _STARCLR resets it.
static uint32_t   star_index = 0;

// Visible stars for the current frame, bucketed by native row.
static uint8_t    star_row_n[STAR_VIS_LINES];
// uint16_t, NOT uint8_t. The star x is STAR_OFFSET_X + (q & 255), i.e.
// 16..271, and 256..271 do not fit in a byte -- they wrapped to 0..15, so
// sixteen of the 256 star columns were drawn down the left edge of the
// screen instead of near the right. A latent bug in the shipped row
// renderer, found only because render_native_column() buckets the same
// value untruncated and the two paths then disagreed by 17 pixels
// (DEVNOTES #82).
static uint16_t   star_row_x[STAR_VIS_LINES][STAR_MAX_PER_ROW];
static uint16_t   star_row_c[STAR_VIS_LINES][STAR_MAX_PER_ROW];

// The same stars bucketed by native COLUMN, for render_native_column().
// Yoko renders a column per scanline, and walking all ~128 visible hits to
// find the handful on one column would undo the point of the hit table.
//
// x is uniform over the raster's 256-wide star window and only two of the
// four sets are visible at once, so the average is well under one star per
// column; 8 is headroom, and the bound check below is kept for the same
// reason its row twin keeps one.
#define STAR_MAX_PER_COL 8
static uint8_t    star_col_n[GALAGA_GAME_WIDTH];
static uint8_t    star_col_y[GALAGA_GAME_WIDTH][STAR_MAX_PER_COL];
static uint16_t   star_col_c[GALAGA_GAME_WIDTH][STAR_MAX_PER_COL];

static inline uint16_t star_lfsr_next(uint16_t l) {
    // Fibonacci form, taps 16/13/11/6 -- transcribed from
    // starfield_05xx_device::get_next_lfsr_state().
    uint16_t bit = (uint16_t)((l >> 0) ^ (l >> 3) ^ (l >> 5) ^ (l >> 10));
    return (uint16_t)((l >> 1) | (bit << 15));
}

static void star_build_tables(void) {
    // Star colours: 2 bits per channel. MAME notes "r/g low bit is n/c and
    // effectively becomes a pulldown", which leaves the SAME resistor
    // network denominator as the core palette -- so the per-bit weights are
    // exactly the 470/220 ohm entries already computed above, reused rather
    // than re-derived.
    static const double RGW[2] = { 70.708344, 151.058735 }; // 470, 220 ohm
    static const double BW[2]  = { 81.304348, 173.695652 }; // 470, 220 ohm
    for (int i = 0; i < 64; i++) {
        int r = (int)(RGW[0] * ((i >> 0) & 1) + RGW[1] * ((i >> 1) & 1) + 0.5);
        int g = (int)(RGW[0] * ((i >> 2) & 1) + RGW[1] * ((i >> 3) & 1) + 0.5);
        int b = (int)(BW[0]  * ((i >> 4) & 1) + BW[1]  * ((i >> 5) & 1) + 0.5);
        star_palette[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }

    star_hit_count = 0;
    uint16_t l = STAR_LFSR_SEED;
    for (uint32_t i = 0; i < STAR_PERIOD; i++) {
        if ((l & STAR_HIT_MASK) == STAR_HIT_VALUE && star_hit_count < STAR_MAX_HITS) {
            // set = bits 10 and 8; colour = the scattered bit-shuffle from
            // draw_starfield(), then inverted.
            uint8_t set = (uint8_t)((((l >> 10) & 1) << 1) | ((l >> 8) & 1));
            uint8_t c   = (uint8_t)(((l >> 5) & 0x07) | ((l << 3) & 0x18) | ((l << 2) & 0x20));
            c = (uint8_t)(~c & 0x3F);
            star_hits[star_hit_count].pos   = (uint16_t)i;
            star_hits[star_hit_count].set   = set;
            star_hits[star_hit_count].color = c;
            star_hit_count++;
        }
        l = star_lfsr_next(l);
    }
    star_index = 0;
}

// Buckets this frame's visible stars by row, then advances the sequence the
// same number of clocks the real chip would have consumed.
// RAMFUNC for the same reason galaga_video_begin_frame() is: it runs on the
// frame path, so an XIP stall here eats into the same budget. (Its one-time
// counterpart star_build_tables() is deliberately NOT marked -- it runs once
// at init, and keeping its 65535-iteration loop in flash saves SRAM.)
GALAGA_VID_RAMFUNC static void star_begin_frame(const galaga_system *sys) {
    for (uint32_t y = 0; y < STAR_VIS_LINES; y++) star_row_n[y] = 0;
    for (uint32_t x = 0; x < (uint32_t)GALAGA_GAME_WIDTH; x++) star_col_n[x] = 0;

    uint8_t ctl = sys->starfield_control;
    if (!((ctl >> 5) & 1)) {      // Q5 = _STARCLR: disabled resets the LFSR
        star_index = 0;
        return;
    }

    uint8_t idx_x = (uint8_t)(ctl & 0x07);
    uint8_t set_a = (uint8_t)((ctl >> 3) & 1);
    uint8_t set_b = (uint8_t)(((ctl >> 4) & 1) | 2);

    uint32_t vis_start = (star_index + STAR_PRE_VIS) % STAR_PERIOD;

    for (int h = 0; h < star_hit_count; h++) {
        if (star_hits[h].set != set_a && star_hits[h].set != set_b) continue;
        uint32_t q = (star_hits[h].pos + STAR_PERIOD - vis_start) % STAR_PERIOD;
        if (q >= STAR_VIS_CLOCKS) continue;          // not on screen this frame
        uint32_t y = q >> 8;
        int32_t  x = STAR_OFFSET_X + (int32_t)(q & 255u);
        if (x < 0 || x >= GALAGA_GAME_WIDTH) continue;
        // Proven unreachable (see STAR_MAX_PER_ROW), kept as a cheap bound
        // check so a future change to the decode can't corrupt memory.
        if (star_row_n[y] >= STAR_MAX_PER_ROW) continue;
        const uint16_t c = star_palette[star_hits[h].color];
        star_row_x[y][star_row_n[y]] = (uint16_t)x;
        star_row_c[y][star_row_n[y]] = c;
        star_row_n[y]++;

        // Same star, bucketed the other way.
        if (star_col_n[x] < STAR_MAX_PER_COL) {
            star_col_y[x][star_col_n[x]] = (uint8_t)y;
            star_col_c[x][star_col_n[x]] = c;
            star_col_n[x]++;
        }
    }

    // One frame consumes pre + visible + post clocks; against a 65535-long
    // sequence that is 65536 + the X-scroll adjustment, i.e. a net drift of
    // 1 + offset. This is the whole scrolling mechanism.
    int32_t adv = (int32_t)(STAR_PRE_VIS + STAR_VIS_CLOCKS + STAR_POST_VIS)
                + STAR_SPEED_X[idx_x];
    star_index = (uint32_t)(((int32_t)star_index + adv) % (int32_t)STAR_PERIOD);
}

// Per-frame latched sprite list, rebuilt once by galaga_video_begin_frame().
//
// Why this exists: render_native_row() runs 224 times per frame, and used
// to re-read and re-decode all 64 sprite entries on EVERY one of those
// rows -- ~14000 full decodes per frame, the overwhelming majority of them
// only to discover the sprite does not intersect this row. On the Fruit
// Jam that was enough to push galaga_run_frame() past its 16.67ms budget
// once the attract mode got busy, starving the DVI scanline queue: the
// display fed correctly for the part of the frame Core 0 kept up with and
// went solid red for the rest, growing as sprite count grew (see
// hal_video_fruitjam.cpp for that fork's starvation behaviour).
//
// Decoding once per frame instead means sprite position/size registers are
// LATCHED at frame start rather than re-read per scanline. Real hardware
// does read them during scanout, so a game that deliberately changed a
// sprite mid-frame would look different -- but Galaga updates sprite RAM
// from its interrupt handlers, not mid-scanout, and the rendered output
// was verified byte-identical against the pre-change renderer across a
// full attract-mode cycle in the host harness
// (arcade_arduino/tools/galaga_host). If a future 8080bw/Namco game in
// this framework does need per-scanline sprite reads, give it its own
// renderer rather than reverting this.
typedef struct {
    int16_t sy, sx;
    int16_t height;
    uint8_t code, color;
    uint8_t flipx, flipy, sizex, sizey;
} galaga_sprite_ent;

static galaga_sprite_ent g_sprites[64];
static int               g_sprite_count = 0;

// The latched sprites bucketed by raster COLUMN, the same trick the
// starfield uses. render_native_column() otherwise tests every one of up to
// 64 sprites on every one of 240 columns; here it visits only the handful
// that actually cover the column.
//
// This is the peak-cost lever, and peak is what matters: the DVI queue holds
// 8 scanlines (~555us) and a RUN of scanlines slower than the 69us line rate
// drains it. Landscape rotation 2 was starving with 2.9ms of average
// headroom because its expensive columns land late in the frame, after the
// vblank slack is spent (DEVNOTES #82).
#define SPR_MAX_PER_COL 64
static uint8_t g_spr_col_n[GALAGA_GAME_WIDTH];
static uint8_t g_spr_col_i[GALAGA_GAME_WIDTH][SPR_MAX_PER_COL];

// Number of sprites the last galaga_video_begin_frame() decoded as
// on-screen. Exposed for the sketch's frame-budget heartbeat: sprite count
// is the main content-driven driver of render cost, so a `work` spike is
// only interpretable next to it.
uint32_t galaga_video_debug_sprite_count(void) { return (uint32_t)g_sprite_count; }

GALAGA_VID_RAMFUNC void galaga_video_begin_frame(const galaga_system *sys) {
    star_begin_frame(sys);

    // Sprite RAM: spriteram/spriteram_2/spriteram_3 are the last 0x80
    // bytes of galaga_ram1/2/3 respectively (offset 0x380 within each 1KB
    // block), 64 sprites at 2 bytes/sprite. Position/flip/size formulas
    // quoted directly from galaga_v.cpp's draw_sprites().
    const uint8_t *sram1 = &sys->ram1[0x380];
    const uint8_t *sram2 = &sys->ram2[0x380];
    const uint8_t *sram3 = &sys->ram3[0x380];
    const bool flip = sys->flip_screen;

    int n = 0;
    for (int offs = 0; offs < 0x80; offs += 2) {
        int sizey = (sram3[offs] >> 3) & 0x01;
        int sy    = 256 - sram2[offs] + 1; // sprites are buffered/delayed by one scanline on real hardware
        sy -= 16 * sizey;
        sy = (sy & 0xFF) - 32; // fix wraparound, verbatim from draw_sprites()
        int height = 16 * (sizey + 1);

        // Reject sprites that cannot touch any visible row -- this is the
        // whole point of the pass, and is exactly the test render_native_row()
        // used to repeat per row.
        if (sy >= GALAGA_GAME_HEIGHT || sy + height <= 0) continue;

        int flipx = sram3[offs] & 0x01;
        int flipy = (sram3[offs] >> 1) & 0x01;
        if (flip) { flipx ^= 1; flipy ^= 1; }

        galaga_sprite_ent *e = &g_sprites[n++];
        e->sy     = (int16_t)sy;
        e->sx     = (int16_t)(sram2[offs + 1] - 40 + 0x100 * (sram3[offs + 1] & 3));
        e->height = (int16_t)height;
        e->code   = (uint8_t)(sram1[offs] & 0x7F);
        e->color  = (uint8_t)(sram1[offs + 1] & 0x3F);
        e->flipx  = (uint8_t)flipx;
        e->flipy  = (uint8_t)flipy;
        e->sizex  = (uint8_t)((sram3[offs] >> 2) & 0x01);
        e->sizey  = (uint8_t)sizey;
    }
    g_sprite_count = n;

    // Bucket them by column, in list order so the draw order (later sprite
    // over earlier) is preserved exactly. YOKO ONLY: tate never reads these
    // buckets, and building them is ~2048 appends a frame that its budget
    // should not be paying for.
    if (sys->rotation != 0 && sys->rotation != 2) return;
    for (uint32_t c = 0; c < (uint32_t)GALAGA_GAME_WIDTH; c++) g_spr_col_n[c] = 0;
    for (int si = 0; si < n; si++) {
        const galaga_sprite_ent *e = &g_sprites[si];
        int x0 = e->sx;
        int x1 = e->sx + 16 * (e->sizex + 1);
        if (x0 < 0) x0 = 0;
        if (x1 > GALAGA_GAME_WIDTH) x1 = GALAGA_GAME_WIDTH;
        for (int c = x0; c < x1; c++) {
            if (g_spr_col_n[c] < SPR_MAX_PER_COL)
                g_spr_col_i[c][g_spr_col_n[c]++] = (uint8_t)si;
        }
    }
}

// PER-LAYER TIMING, opt-in and zero-cost when off:
//   --build-property compiler.cpp.extra_flags=-DGALAGA_LAYER_TRACE=1
//
// This exists because five performance hypotheses about this renderer were
// wrong in a row (DEVNOTES #83). `render_max` says a scanline costs 310us
// and says nothing about which of the three layers spent it. Both the frame
// TOTALS and the breakdown of the single worst scanline are kept, because
// the totals say where the frame goes and the worst scanline is what
// actually starves the DVI queue (#35).
//
// Read the numbers knowing the instrument perturbs them: six micros() calls
// per scanline is ~0.2ms a frame, and DEVNOTES #78 records counters far
// cheaper than this moving `starve` on their own.
#if defined(GALAGA_LAYER_TRACE) && (defined(ARDUINO_ARCH_RP2040) || defined(PICO_ON_DEVICE))
#include <Arduino.h> // micros()
#define LT_NOW() micros()
#define LT_ADD(acc, t0) do { (acc) += micros() - (t0); } while (0)
static uint32_t g_lt_star, g_lt_spr, g_lt_tile;          // frame totals
static uint32_t g_lt_worst, g_lt_w_star, g_lt_w_spr, g_lt_w_tile; // worst scanline
#define LT_SCANLINE(st, sp, ti) do {                                      \
    const uint32_t tot = (st) + (sp) + (ti);                              \
    g_lt_star += (st); g_lt_spr += (sp); g_lt_tile += (ti);               \
    if (tot > g_lt_worst) { g_lt_worst = tot; g_lt_w_star = (st);         \
                            g_lt_w_spr = (sp); g_lt_w_tile = (ti); }      \
} while (0)
#else
#define LT_NOW() 0u
#define LT_ADD(acc, t0) do { (void)(t0); } while (0)
#define LT_SCANLINE(st, sp, ti) do { } while (0)
#endif

void galaga_debug_take_layers(uint32_t *star_us, uint32_t *spr_us, uint32_t *tile_us,
                              uint32_t *worst, uint32_t *w_star, uint32_t *w_spr,
                              uint32_t *w_tile) {
#if defined(GALAGA_LAYER_TRACE) && (defined(ARDUINO_ARCH_RP2040) || defined(PICO_ON_DEVICE))
    *star_us = g_lt_star; *spr_us = g_lt_spr; *tile_us = g_lt_tile;
    *worst = g_lt_worst; *w_star = g_lt_w_star; *w_spr = g_lt_w_spr; *w_tile = g_lt_w_tile;
    g_lt_star = g_lt_spr = g_lt_tile = 0;
    g_lt_worst = g_lt_w_star = g_lt_w_spr = g_lt_w_tile = 0;
#else
    *star_us = *spr_us = *tile_us = 0;
    *worst = *w_star = *w_spr = *w_tile = 0;
#endif
}

// `reverse_x` writes the row right-to-left. It is a purely GEOMETRIC
// transform for rotation 3 and is deliberately kept separate from
// `flip_screen`, which is emulated game state and is NOT interchangeable
// with it: flip_screen additionally reverses the row index and selects the
// hardware's second, pre-x-flipped character set (the `| 0x80` on the tile
// code below). Only the output x-order composes, hence `rev` = the XOR of
// the two, while `eff_y` and the tile code stay keyed on `flip` alone.
GALAGA_VID_RAMFUNC static void render_native_row(const galaga_system *sys, uint32_t native_y, uint16_t *out, bool reverse_x) {
    bool flip = sys->flip_screen;
    bool rev  = flip ^ reverse_x;
    uint32_t eff_y = flip ? (uint32_t)(GALAGA_GAME_HEIGHT - 1) - native_y : native_y;

    const uint32_t lt0 = LT_NOW();
    uint32_t lt_star = 0, lt_spr = 0, lt_tile = 0;

    // Background: black, then the starfield. Drawn FIRST so sprites and the
    // tilemap paint over it, matching screen_update_galaga()'s order
    // (starfield -> sprites -> tilemap).
    for (uint32_t x = 0; x < (uint32_t)GALAGA_GAME_WIDTH; x++) out[x] = 0;
    if (eff_y < STAR_VIS_LINES) {
        uint8_t sn = star_row_n[eff_y];
        for (uint8_t i = 0; i < sn; i++) {
            uint32_t sx = star_row_x[eff_y][i];
            if (rev) sx = (uint32_t)(GALAGA_GAME_WIDTH - 1) - sx;
            out[sx] = star_row_c[eff_y][i];
        }
    }

    LT_ADD(lt_star, lt0);
    const uint32_t lt1 = LT_NOW();

    // Sprites -- decoded once per frame, see galaga_video_begin_frame().
    for (int si = 0; si < g_sprite_count; si++) {
        const galaga_sprite_ent *e = &g_sprites[si];
        int local_y = (int)eff_y - e->sy;
        if (local_y < 0 || local_y >= e->height) continue;

        int code   = e->code;
        int color  = e->color;
        int sx     = e->sx;
        int flipx  = e->flipx;
        int flipy  = e->flipy;
        int sizex  = e->sizex;
        int sizey  = e->sizey;

        int cell_row = local_y >> 4;
        int in_cell_y = local_y & 15;
        int gfx_row = cell_row ^ (sizey * flipy);
        int py2 = flipy ? 15 - in_cell_y : in_cell_y;

        const uint16_t *spen = sprite_pen_rgb[color];
        for (int cell_col = 0; cell_col <= sizex; cell_col++) {
            int gfx_col = cell_col ^ (sizex * flipx);
            int sub_code = (code + GFX_OFFS[gfx_row][gfx_col]) & (NUM_SPRITES - 1);
            int sx_cell = sx + 16 * cell_col;

            for (int col = 0; col < 16; col++) {
                int x = sx_cell + col;
                if (x < 0 || x >= GALAGA_GAME_WIDTH) continue;
                int px2 = flipx ? 15 - col : col;
                uint8_t pixel2 = sprite_pixels[sub_code][px2][py2];
                if (pixel2 == 0) continue; // transparent
                uint32_t out_x = rev ? (uint32_t)(GALAGA_GAME_WIDTH - 1) - (uint32_t)x : (uint32_t)x;
                out[out_x] = spen[pixel2];
            }
        }
    }

    // Tilemap, drawn last (on top of sprites) -- see this function's
    // header comment for the draw-order citation and the transparency
    // approximation. Tile code: videoram[idx] & 0x7F, |0x80 when
    // flip_screen (selects the hardware's second, pre-x-flipped
    // character set instead of software-flipping); color: videoram[idx +
    // 0x400] & 0x3F. Verified against get_tile_info().
    LT_ADD(lt_spr, lt1);
    const uint32_t lt2 = LT_NOW();

    uint32_t row = eff_y >> 3, py = eff_y & 7u;
    for (uint32_t col = 0; col < 36; col++) {
        uint16_t idx = scan_rows(col, row);
        uint8_t code6  = (uint8_t)((sys->video_ram[idx] & 0x7Fu) | (flip ? 0x80u : 0u));
        uint8_t color6 = (uint8_t)(sys->video_ram[idx + 0x400u] & 0x3Fu);
        // Hoisted out of the pixel loop: the pen table for this tile's
        // colour, and a pointer to this row of the decoded tile (x stride
        // is 8 in tile_pixels[code][x][y]).
        const uint16_t *pen = char_pen_rgb[color6];
        const uint8_t  *tp  = &tile_pixels[code6][0][py];
        if (!rev) {
            uint16_t *o = &out[col * 8];
            for (uint32_t px = 0; px < 8; px++) {
                uint8_t pixel2 = tp[px * 8];
                if (pixel2) o[px] = pen[pixel2]; // 0 = transparent, see header comment
            }
        } else {
            for (uint32_t px = 0; px < 8; px++) {
                uint8_t pixel2 = tp[px * 8];
                if (pixel2) out[(uint32_t)(GALAGA_GAME_WIDTH - 1) - (col * 8 + px)] = pen[pixel2];
            }
        }
    }

    LT_ADD(lt_tile, lt2);
    LT_SCANLINE(lt_star, lt_spr, lt_tile);
}

// The transpose of render_native_row(): one native COLUMN of starfield,
// sprites and tilemap, in the same draw order. Yoko needs this because a
// physical scanline there IS a raster column, and a row-only renderer
// cannot emit one until every row exists -- which is what forced the 129KB
// frame_cache and the whole-frame burst (DEVNOTES #18/#79).
//
// No `reverse_x` twin here, unlike the row renderer. That parameter exists
// so rotation 3 can write a reversed row straight into the scanline buffer;
// in yoko the reversal is along the EMIT axis instead, so it belongs to
// av_emit_row_merge_rev() and not in here.
//
// Galaga draws all its latched sprites on every line with no per-line
// arbitration, which is what makes a straight transposition possible.
// `rev_out` reverses the OUTPUT index, so the caller can write straight
// into the scanline buffer for the mirrored orientations instead of going
// through a scratch column and an emit pass -- the same shortcut the tate
// fast path takes, and worth ~1.3ms a frame here.
//
// `merge` skips the initial clear, which turns a second call into a MERGE
// of two adjacent raster columns for free: every layer below already writes
// only non-transparent pixels, so the clear is the ONLY thing that would
// erase the previous column. Yoko collapses 288 raster columns onto 240
// canvas rows and dropping the odd one out deletes 1-pixel features
// (DEVNOTES #80), so the merge is required -- doing it this way costs
// nothing beyond the second render itself.
GALAGA_VID_RAMFUNC static void render_native_column(const galaga_system *sys,
                                                    uint32_t native_x, uint16_t *out,
                                                    bool rev_out, bool merge) {
    const bool flip = sys->flip_screen;
    const bool orev = flip ^ rev_out;
    // The row renderer maps internal x to display x with `rev`; with no
    // reverse_x that is just `flip`, so invert it once here.
    const uint32_t x = flip ? (uint32_t)(GALAGA_GAME_WIDTH - 1) - native_x : native_x;

    // Background: black, then the starfield -- drawn FIRST so sprites and
    // the tilemap paint over it, matching screen_update_galaga()'s order.
    if (!merge)
        for (uint32_t i = 0; i < (uint32_t)GALAGA_GAME_HEIGHT; i++) out[i] = 0;
    {
        const uint8_t sn = star_col_n[x];
        for (uint8_t i = 0; i < sn; i++) {
            const uint32_t ey = star_col_y[x][i];
            out[orev ? (uint32_t)(GALAGA_GAME_HEIGHT - 1) - ey : ey] = star_col_c[x][i];
        }
    }

    // Sprites -- decoded once per frame, see galaga_video_begin_frame().
    const uint8_t sc_n = g_spr_col_n[x];
    const uint8_t *sc_i = g_spr_col_i[x];
    for (uint8_t k = 0; k < sc_n; k++) {
        const galaga_sprite_ent *e = &g_sprites[sc_i[k]];
        const uint16_t *spen = sprite_pen_rgb[e->color];

        for (int cell_col = 0; cell_col <= e->sizex; cell_col++) {
            const int col = (int)x - (e->sx + 16 * cell_col);
            if (col < 0 || col >= 16) continue;   // this cell misses the column
            const int gfx_col = cell_col ^ (e->sizex * e->flipx);
            const int px2 = e->flipx ? 15 - col : col;

            for (int local_y = 0; local_y < e->height; local_y++) {
                const int eff_y = e->sy + local_y;
                if (eff_y < 0 || eff_y >= GALAGA_GAME_HEIGHT) continue;
                const int gfx_row = (local_y >> 4) ^ (e->sizey * e->flipy);
                const int py2 = e->flipy ? 15 - (local_y & 15) : (local_y & 15);
                const int sub_code = (e->code + GFX_OFFS[gfx_row][gfx_col]) & (NUM_SPRITES - 1);
                const uint8_t pixel2 = sprite_pixels[sub_code][px2][py2];
                if (pixel2 == 0) continue; // transparent
                out[orev ? (uint32_t)(GALAGA_GAME_HEIGHT - 1) - (uint32_t)eff_y
                         : (uint32_t)eff_y] = spen[pixel2];
            }
        }
    }

    // Tilemap, drawn last (on top of sprites). Note this transposes in our
    // favour: tile_pixels is [code][x][y], so a COLUMN of a tile is
    // contiguous where the row renderer has to stride by 8.
    const uint32_t tcol = x >> 3, px = x & 7u;
    for (uint32_t row = 0; row < (uint32_t)GALAGA_GAME_HEIGHT / 8u; row++) {
        const uint16_t idx = scan_rows(tcol, row);
        const uint8_t code6  = (uint8_t)((sys->video_ram[idx] & 0x7Fu) | (flip ? 0x80u : 0u));
        const uint8_t color6 = (uint8_t)(sys->video_ram[idx + 0x400u] & 0x3Fu);
        const uint16_t *pen = char_pen_rgb[color6];
        const uint8_t  *tp  = &tile_pixels[code6][px][0];
        for (uint32_t py = 0; py < 8; py++) {
            const uint8_t pixel2 = tp[py];
            if (!pixel2) continue; // 0 = transparent, see header comment
            const uint32_t ey = row * 8u + py;
            out[orev ? (uint32_t)(GALAGA_GAME_HEIGHT - 1) - ey : ey] = pen[pixel2];
        }
    }
}

// Renders the group of raster columns that canvas row `dvi_y` collapses,
// merged, into `out`. The merge is free: the second and later columns are
// rendered with `merge` set, so they overlay the first instead of clearing
// it. No scratch buffer and no second pass.
GALAGA_VID_RAMFUNC static void render_native_column_group(const galaga_system *sys,
                                                          uint32_t first, int step,
                                                          uint32_t count, uint16_t *out,
                                                          bool rev_out) {
    render_native_column(sys, first, out, rev_out, false);
    for (uint32_t k = 1; k < count; k++)
        render_native_column(sys, (uint32_t)((int)first + step * (int)k), out, rev_out, true);
}

// THE 129KB frame_cache THAT USED TO LIVE HERE IS GONE. It existed only
// because this file could render rows and a yoko scanline needs a COLUMN,
// which forced a whole-frame burst before the first
// hal_video_acquire_scanline() and starved the DVI queue every frame
// (DEVNOTES #18/#75/#79). render_native_column() above removed the reason
// for it. The cache was a static, so it had been costing its full 129KB in
// every orientation including the tate default, where it was never read.

GALAGA_VID_RAMFUNC void galaga_video_render_scanline(const galaga_system *sys, uint32_t dvi_y, uint16_t *buf) {
    bool mir = sys->mirror_x;

    // Fast path for the default tate rotation. This function runs 240x per
    // frame, and the general form below cost three full passes over the
    // pixels every time: memset the whole 640-pixel line, let
    // render_native_row() clear its own 288-entry scratch row, then copy
    // that row into the line. Rotation 1 is a straight 1:1 copy, so it can
    // render DIRECTLY into the caller's buffer and clear only the side
    // borders -- removing one clear and one copy per scanline. Measured on
    // the Fruit Jam as real frame-budget headroom, which is what keeps the
    // DVI scanline queue fed (see hal_video_fruitjam.cpp for what happens
    // when it isn't).
    //
    // BOTH portrait rotations take this path. Rotation 3 used to fall
    // through to the general switch below and cost a full 640-pixel clear
    // plus a reversed scratch-row copy on every scanline -- and when this
    // machine's default changed from 1 to 3 (see galaga_init()), that was
    // instantly enough to blow the ~3ms of headroom and put red bars back
    // on the screen on real hardware. It renders reversed directly instead
    // now, via render_native_row()'s reverse_x, at the same cost as
    // rotation 1. **Any rotation that is a game default must have a fast
    // path; the general switch is for the two landscape modes, which are
    // not defaults and already carry their own known stall risk.**
    if (sys->rotation == 1 || sys->rotation == 3) {
        if (dvi_y < av_tate.y0 || dvi_y >= av_tate.y1) {
            memset(buf, 0, HAL_VIDEO_WIDTH * sizeof(uint16_t));
            return;
        }
        const uint32_t d = av_tate.row[dvi_y];
        // Rotation 3 is rotation 1 with both axes reversed, so the row index
        // reverses here and the column order reverses below.
        uint32_t dx;
        if (sys->rotation == 1) dx = mir ? (uint32_t)(GALAGA_GAME_HEIGHT - 1) - d : d;
        else                    dx = mir ? d : (uint32_t)(GALAGA_GAME_HEIGHT - 1) - d;

        if (av_tate.col_1to1) {
            // The direct path described above: render straight into the
            // caller's buffer, clearing only the side borders. Available
            // only while the column map is an identity shift -- see
            // av_map_t::col_1to1.
            memset(buf, 0, av_tate.x0 * sizeof(uint16_t));
            memset(buf + av_tate.x1, 0,
                   (HAL_VIDEO_WIDTH - av_tate.x1) * sizeof(uint16_t));
            render_native_row(sys, dx, buf + av_tate.x0, sys->rotation == 3);
            return;
        }

        // Aspect-corrected: the raster no longer lands one-to-one on the
        // canvas, so it has to go through a scratch row. This costs the
        // extra clear-and-copy that DEVNOTES #33 measured as enough to blow
        // this game's headroom -- Galaga peaks at 14946us of 16660us, so
        // MEASURE ON HARDWARE before shipping the stretch on as this
        // machine's default.
        static uint16_t scratch[GALAGA_GAME_WIDTH];

        // Skip the render when this canvas row repeats the previous one --
        // the 224->240 upsample makes 16 of 240 rows duplicates, and they
        // are adjacent. See av_map_t::rep and DEVNOTES #78.
        static uint32_t last_dx = 0xFFFFFFFFu;
        if (dvi_y == av_tate.y0) last_dx = 0xFFFFFFFFu; // new frame
        if (dx != last_dx) { render_native_row(sys, dx, scratch, false); last_dx = dx; }

        memset(buf, 0, av_tate.x0 * sizeof(uint16_t));
        memset(buf + av_tate.x1, 0, (HAL_VIDEO_WIDTH - av_tate.x1) * sizeof(uint16_t));
        // Source-driven emit rather than `buf[x] = scratch[col[x]]`: one
        // load and two stores per raster sample, nothing dependent. The
        // reversal for rotation 3 is done here rather than inside
        // render_native_row(), so the memoised row above is orientation-
        // independent and both rotations share it.
        if (sys->rotation == 1) av_emit_row(buf, scratch, &av_tate);
        else                    av_emit_row_rev(buf, scratch, &av_tate);
        return;
    }

    static uint16_t colbuf[GALAGA_GAME_HEIGHT]; // yoko's on-demand column

    // Yoko clears only the PILLARBOX. render_native_column() writes every
    // pixel of the picture itself, so clearing the full width here would
    // write 224 of the 320 columns twice -- 53,760 redundant stores a
    // frame, on the SRAM bus Core 1's DMA is competing for. The tate path
    // below has always cleared only its borders, for the same reason.
    if (sys->rotation == 0 || sys->rotation == 2) {
        memset(buf, 0, av_yoko.x0 * sizeof(uint16_t));
        memset(buf + av_yoko.x1, 0,
               (HAL_VIDEO_WIDTH - av_yoko.x1) * sizeof(uint16_t));
    } else {
        memset(buf, 0, HAL_VIDEO_WIDTH * sizeof(uint16_t));
    }

    switch (sys->rotation) {

    case 0: { // landscape -- see arcade_video_geom.h; yoko NARROWS the short axis
        const uint32_t dy  = av_yoko.row[dvi_y];
        const uint32_t col = (uint32_t)(GALAGA_GAME_WIDTH - 1) - dy;
        if (av_yoko.col_1to1) {
            // Straight into the scanline buffer, no scratch and no emit --
            // the same shortcut the tate path above takes.
            // ONE COLUMN PER CANVAS ROW, NOT THE MERGED GROUP -- unlike
            // Pac-Man and Ms. Pac-Man. Yoko collapses 288 raster columns
            // onto 240 canvas rows, and merging the collapsed one back in
            // costs a second full column render on 48 of the 240
            // scanlines. Measured on hardware (DEVNOTES #82): that doubles
            // those scanlines' cost, and `starve` goes from <=40 a window
            // to 1806 -- visible red lines -- even though `work_max` only
            // moves 14236us -> 14964us. Classic #35: the DVI queue cares
            // about the DISTRIBUTION inside a frame, not the total.
            //
            // The merge is worth much less here than it is on the Namco
            // maze games: +12.9%/+6.7% of the lit pixels, against Pac-Man's
            // +41%, because Galaga's content is 16-pixel sprites and 8-pixel
            // characters rather than 1-pixel maze walls, so a dropped column
            // thins features instead of deleting them. Red lines are worse
            // than slightly thinner sprites, so this ships un-merged.
            //
            // To get both, the extra column would have to be rendered on a
            // NEIGHBOURING scanline that has slack rather than on the one
            // that needs it -- a pipeline, not a bigger buffer.
            render_native_column_group(sys, col, -1, 1u,
                                       buf + av_yoko.x0, mir);
        } else {
            render_native_column_group(sys, col, -1, 1u, colbuf, false);
            if (mir) av_emit_row_merge_rev(buf, colbuf, &av_yoko);
            else     av_emit_row_merge(buf, colbuf, &av_yoko);
        }
        break;
    }

    case 2: { // 180 deg
        const uint32_t col = av_yoko.row[dvi_y];
        if (av_yoko.col_1to1) {
            // One column per canvas row -- see case 0's note.
            render_native_column_group(sys, col, +1, 1u,
                                       buf + av_yoko.x0, !mir);
        } else {
            render_native_column_group(sys, col, +1, 1u, colbuf, false);
            if (mir) av_emit_row_merge(buf, colbuf, &av_yoko);
            else     av_emit_row_merge_rev(buf, colbuf, &av_yoko);
        }
        break;
    }

    default: break;
    }
}

void galaga_draw_error_frame(uint16_t color) {
    for (uint32_t i = 0; i < HAL_VIDEO_HEIGHT; i++) {
        uint16_t *buf = hal_video_acquire_scanline();
        for (uint32_t x = 0; x < HAL_VIDEO_WIDTH; x++) buf[x] = color;
        hal_video_submit_scanline(buf);
    }
}
