// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Donkey Kong tile+sprite renderer.
//
// Every hardware fact below was verified against MAME's dkong_v.cpp and
// dkong.cpp, plus src/emu/video/resnet.cpp for the palette maths (upstream
// mamedev/mame), not inferred:
//
// - Tile format: GFXDECODE_ENTRY("gfx1", 0, gfx_8x8x2_planar, 0, 64) --
//   8x8, 2bpp, the two bitplanes in separate halves of the 0x1000 region.
// - Sprite format: `spritelayout` in dkong.cpp -- 16x16, 2bpp, 128 sprites,
//   with BOTH the bitplanes AND the left/right halves of each sprite living
//   in separate quarters of the 0x2000 region. Two independent splits in
//   one layout; see decode_sprites() for the arithmetic.
// - Colour codes: dkong_bg_tile_info() -- the per-tile colour comes from a
//   PROM, not colour RAM, and its index folds four tile rows onto one PROM
//   row: color_codes[tile_index % 32 + 32 * (tile_index / 32 / 4)].
// - Sprite selection and placement: draw_sprites(), called once per
//   scanline through update_partial() from scanline_callback().
// - Palette: dkong2b_palette() -> compute_res_net_all() -> compute_res_net().
//
// COORDINATES. This machine's tilemap is 32x32 tiles = 256x256 pixels, but
// the visible window is only 224 lines tall: MAME's dkong.h gives VBEND=16
// and VBSTART=240. So native row 0 of the picture is tilemap/screen line
// 16, and sprite Y arithmetic below works in SCREEN coordinates (16..239)
// rather than picture coordinates (0..223). Mixing those two up shifts
// sprites 16 pixels against the background, which reads as "sprites are
// slightly wrong" rather than as an obvious fault.
#include <math.h>
#include <string.h>
#include "dkong_video.h"
#include "arcade_hal_video.h"
#include "arcade_video_geom.h"

// Border/scale constants for a 640x480 4:3 monitor, same derivation as
// every other renderer here (see invaders_video.cpp's header). Only the
// first 320 of each 640-pixel scanline are displayed -- libdvi's 16bpp path
// encodes h_active_pixels/2 source pixels across the full line -- so these
// are computed against a 320-wide visible axis. See tools/README.md.
// Tate: native rows (224) along DVI y x2 = 448; native cols (256) along DVI x x1.

// Visible window in tilemap/screen coordinates (MAME dkong.h VBEND/VBSTART).
#define DKONG_VBEND 16u

uint8_t dkong_gfx1[DKONG_GFX1_SIZE];
uint8_t dkong_gfx2[DKONG_GFX2_SIZE];
uint8_t dkong_palette_prom[DKONG_PALETTE_PROM_SIZE];
uint8_t dkong_color_prom[DKONG_COLOR_PROM_SIZE];

// Decoded pen indices (0..3), built once at load. Costs 16KB + 32KB of
// SRAM and buys a table lookup per pixel instead of two planar bit
// extractions -- the same trade ArcadeMachine_Pacman makes, and lever 2 in
// the port playbook.
static uint8_t tile_pixels[256][8][8];    // [code][x][y]
static uint8_t sprite_pixels[128][16][16]; // [code][x][y]
static uint16_t rgb565_palette[256];

static uint32_t g_sprite_peak = 0, g_sprite_limit_hits = 0;

void dkong_video_debug_take_sprite_stats(uint32_t *out_peak, uint32_t *out_limit_hits) {
    if (out_peak)       *out_peak       = g_sprite_peak;
    if (out_limit_hits) *out_limit_hits = g_sprite_limit_hits;
    g_sprite_peak = 0;
    g_sprite_limit_hits = 0;
}

// --- palette -------------------------------------------------------------
//
// A direct transcription of compute_res_net() (src/emu/video/resnet.cpp)
// specialised to dkong_net_info's exact configuration, which dkong_v.cpp
// declares as:
//
//   options: RES_NET_VCC_5V | RES_NET_VBIAS_5V | RES_NET_VIN_MB7052
//            | RES_NET_MONITOR_SANYO_EZV20
//   R: { RES_NET_AMP_DARLINGTON, rBias 470, rGnd 0, 3 bits, {1000,470,220} }
//   G: { RES_NET_AMP_DARLINGTON, rBias 470, rGnd 0, 3 bits, {1000,470,220} }
//   B: { RES_NET_AMP_EMITTER,    rBias 680, rGnd 0, 2 bits, { 470,220,  0} }
//
// Specialised rather than ported wholesale because the general solver
// carries a dozen branches for configurations this board does not use, and
// every one of them would be dead code that still has to be read and
// trusted. The constants that survive are named after their MAME source so
// the correspondence stays checkable. This runs 256 times, once, at load --
// its cost is irrelevant, so it is written for clarity.
#define TTL_VOL 0.05
#define TTL_VOH 4.00
#define RES_NET_VCC 5.0

// RES_NET_VIN_MB7052 is RES_NET_VIN_TTL_OUT (resnet.h), whose comment
// derives this from the 82s129/7052 datasheet as roughly 1.4k / 30.
#define TTL_HRES 50.0

static double res_net_channel(int inputs, int nbits, const double *R,
                              double rBias, double minout, double cut) {
    double rTotal = 0.0, v = 0.0;
    int open_col = 0;

    // Pass one: the LOW inputs (transistor conducting to ground).
    for (int i = 0; i < nbits; i++) {
        if (R[i] != 0.0 && !((inputs >> i) & 1)) {
            rTotal += 1.0 / R[i];
            v      += TTL_VOL / R[i];
        }
    }
    if (rBias != 0.0) { rTotal += 1.0 / rBias; v += RES_NET_VCC / rBias; } // vBias = 5V
    // rGnd is 0 for every dkong channel, so the rGnd term of the original
    // is omitted rather than written as a no-op.

    // "if the resulting voltage after application of all low inputs is
    //  greater than vOH, treat high inputs as open collector" -- resnet.cpp
    if (v / rTotal > TTL_VOH) open_col = 1;

    // Pass two: the HIGH inputs.
    if (!open_col) {
        for (int i = 0; i < nbits; i++) {
            if (R[i] != 0.0 && ((inputs >> i) & 1)) {
                rTotal += 1.0 / (R[i] + TTL_HRES);
                v      += TTL_VOH / (R[i] + TTL_HRES);
            }
        }
    }

    rTotal = 1.0 / rTotal;
    v *= rTotal;
    v = (v - cut) > minout ? (v - cut) : minout;

    // RES_NET_MONITOR_SANYO_EZV20
    v = RES_NET_VCC - v;
    if (v < 0.7) v = 0.7;
    v -= 0.7;
    double top = RES_NET_VCC - 2 * 0.7;
    if (v > top) v = top;
    v = v / (RES_NET_VCC - 1.4) * RES_NET_VCC;

    return v;
}

static void build_palette(void) {
    static const double R_RG[3] = { 1000.0, 470.0, 220.0 };
    static const double R_B[3]  = {  470.0, 220.0,   0.0 };

    // RES_NET_AMP_DARLINGTON / RES_NET_AMP_EMITTER as applied by the
    // PER-CHANNEL switch in compute_res_net(). Note the per-channel
    // darlington uses minout 0.7 where the GLOBAL one uses 0.9 -- dkong
    // specifies its amplifiers per channel, so 0.7 is the right constant
    // here. That two-decimal difference is exactly the sort of thing that
    // silently shifts every colour slightly.
    const double MINOUT_DARLINGTON = 0.7, CUT_DARLINGTON = 0.0;
    const double MINOUT_EMITTER    = 0.0, CUT_EMITTER    = 0.7;

    int r8[256], g8[256], b8[256];

    for (int i = 0; i < 256; i++) {
        // res_net_decode_info dkong_decode_info: two PROMs, c-2k.bpr at
        // offset 0 (low nibble) and c-2j.bpr at offset 256 (high nibble).
        uint8_t lo = dkong_palette_prom[i];
        uint8_t hi = dkong_palette_prom[i + 256];

        int rbits = (hi >> 1) & 0x07;
        int gbits = ((hi << 2) & 0x04) | ((lo >> 2) & 0x03);
        int bbits = (lo >> 0) & 0x03;

        double r = res_net_channel(rbits, 3, R_RG, 470.0, MINOUT_DARLINGTON, CUT_DARLINGTON);
        double g = res_net_channel(gbits, 3, R_RG, 470.0, MINOUT_DARLINGTON, CUT_DARLINGTON);
        double b = res_net_channel(bbits, 2, R_B,  680.0, MINOUT_EMITTER,    CUT_EMITTER);

        // "Now treat tri-state black background generation" -- dkong2b_palette().
        // Any entry whose low two bits are zero NORs to CS=1, tri-states the
        // output and gives real black regardless of what the PROMs hold.
        if ((i & 0x03) == 0x00) { r = g = b = 0.0; }

        r8[i] = (int)(r * 255 / RES_NET_VCC + 0.4);
        g8[i] = (int)(g * 255 / RES_NET_VCC + 0.4);
        b8[i] = (int)(b * 255 / RES_NET_VCC + 0.4);
    }

    // palette.palette()->normalize_range(0, 255): rescale so the brightest
    // component in the range becomes 255. Without it the whole picture is
    // noticeably dim, which looks like a monitor problem rather than a
    // missing line of code.
    int peak = 1;
    for (int i = 0; i < 256; i++) {
        if (r8[i] > peak) peak = r8[i];
        if (g8[i] > peak) peak = g8[i];
        if (b8[i] > peak) peak = b8[i];
    }
    for (int i = 0; i < 256; i++) {
        int r = r8[i] * 255 / peak, g = g8[i] * 255 / peak, b = b8[i] * 255 / peak;
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        rgb565_palette[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
}

// --- gfx decode ----------------------------------------------------------

// gfx_8x8x2_planar: 8x8, 2 planes at RGN_FRAC(1,2) and RGN_FRAC(0,2) --
// i.e. plane 0 (the HIGH bit) in the upper half of the region, plane 1 in
// the lower half. x offsets STEP8(0,1) (bits within a byte, MSB first),
// y offsets STEP8(0,8), 8 bytes per plane per tile.
static void decode_tiles(void) {
    const uint32_t plane0 = DKONG_GFX1_SIZE / 2; // RGN_FRAC(1,2), in bytes
    for (int code = 0; code < 256; code++) {
        for (int y = 0; y < 8; y++) {
            uint8_t b0 = dkong_gfx1[plane0 + code * 8 + y];
            uint8_t b1 = dkong_gfx1[         code * 8 + y];
            for (int x = 0; x < 8; x++) {
                uint8_t bit = (uint8_t)(7 - x); // MSB is the leftmost pixel
                tile_pixels[code][x][y] = (uint8_t)((((b0 >> bit) & 1) << 1) |
                                                     ((b1 >> bit) & 1));
            }
        }
    }
}

// `spritelayout`: 16x16, 2bpp, RGN_FRAC(1,4) = 128 sprites.
//   planes:  { RGN_FRAC(1,2), RGN_FRAC(0,2) }  = bit offsets 0x8000 and 0
//   x:       { STEP8(0,1), STEP8(RGN_FRAC(1,4),1) } -- the left 8 columns are
//            at bit offset 0, the right 8 at bit offset RGN_FRAC(1,4)=0x4000
//   y:       { STEP16(0,8) }
//   stride:  16*8 bits = 16 bytes per sprite per quarter
//
// So the region is quartered TWICE over: plane x half. All offsets in
// MAME's layouts are BIT offsets, which is why everything below is computed
// in bits and only divided by 8 at the final fetch.
static void decode_sprites(void) {
    const uint32_t region_bits = DKONG_GFX2_SIZE * 8;
    const uint32_t frac2 = region_bits / 2; // RGN_FRAC(1,2) -- plane 0
    const uint32_t frac4 = region_bits / 4; // RGN_FRAC(1,4) -- right half
    const uint32_t stride = 16 * 8;         // bits per sprite per quarter

    for (int code = 0; code < 128; code++) {
        for (int y = 0; y < 16; y++) {
            for (int x = 0; x < 16; x++) {
                uint32_t xoff = (x < 8) ? (uint32_t)x : frac4 + (uint32_t)(x - 8);
                uint32_t yoff = (uint32_t)y * 8;
                uint32_t base = (uint32_t)code * stride + yoff + xoff;

                uint32_t a0 = frac2 + base; // plane 0 -- the HIGH bit
                uint32_t a1 = base;         // plane 1
                uint8_t p0 = (uint8_t)((dkong_gfx2[a0 >> 3] >> (7 - (a0 & 7))) & 1);
                uint8_t p1 = (uint8_t)((dkong_gfx2[a1 >> 3] >> (7 - (a1 & 7))) & 1);
                sprite_pixels[code][x][y] = (uint8_t)((p0 << 1) | p1);
            }
        }
    }
}

void dkong_video_build_caches(void) {
    build_palette();
    decode_tiles();
    decode_sprites();
}

// --- rendering -----------------------------------------------------------

// GFXDECODE_ENTRY(..., 0, 64): 64 colour codes of 4 pens each, covering all
// 256 palette entries. So the palette index is simply colour*4 + pen.
static inline uint16_t pen_color(uint8_t color, uint8_t pixel) {
    return rgb565_palette[(uint8_t)(((color & 0x3F) << 2) | (pixel & 0x03))];
}

// Renders one native picture row (0..DKONG_GAME_HEIGHT-1) of tilemap +
// sprites into `out` (DKONG_GAME_WIDTH RGB565 pixels).
static void render_native_row(const dkong_system *sys, uint32_t native_y, uint16_t *out) {
    const bool flip = sys->flip_screen;
    const uint32_t screen_y = native_y + DKONG_VBEND;

    // tilemap().set_flip_all(FLIPX|FLIPY) flips the whole 256x256 tilemap
    // image, which is a coordinate transform on both axes.
    const uint32_t ty = flip ? (255u - screen_y) : screen_y;
    const uint32_t row = ty >> 3, py = ty & 7u;

    // Background tilemap: 32 columns of 8 pixels = the full 256-pixel width.
    for (uint32_t col = 0; col < 32; col++) {
        uint32_t tcol = flip ? (31u - col) : col;
        uint16_t tile_index = (uint16_t)(row * 32u + tcol);
        uint8_t  code  = sys->video_ram[tile_index];
        uint8_t  color = (uint8_t)((dkong_color_prom[(tile_index % 32u) + 32u * (tile_index / 32u / 4u)] & 0x0F)
                                   + 0x10 * sys->palette_bank);
        for (uint32_t px = 0; px < 8; px++) {
            uint32_t tx = flip ? (7u - px) : px;
            out[col * 8u + px] = pen_color(color, tile_pixels[code][tx][py]);
        }
    }

    // Sprites. Transcribed from draw_sprites(bitmap, cliprect, 0x40, 1),
    // which real hardware evaluates once per scanline against a 64x9 line
    // buffer -- hence the hard limit of 16 per line, which is emulated here
    // rather than ignored because the game relies on it.
    int scanline_vf, scanline_vfc, add_y, add_x;
    const int scanline = (int)(screen_y & 0xFFu);
    scanline_vf = scanline_vfc = (int)((screen_y - 1) & 0xFFu);
    if (flip) {
        scanline_vf  ^= 0xFF;
        scanline_vfc ^= 0xFF;
        add_y = 0xF7; add_x = 0xF7;
    } else {
        add_y = 0xF9; add_x = 0xF7;
    }

    const int base = sys->sprite_bank << 9;
    int num_sprt = 0;
    for (int offs = base; num_sprt < 16 && offs < base + 0x200; offs += 4) {
        int y = sys->sprite_ram[offs];
        if (((y + add_y + 1 + scanline_vf) & 0xF0) != 0xF0) continue;

        // MAME's gfx_element wraps the code modulo the element count. The
        // 0x40 bank bit that feeds this is documented in draw_sprites() as
        // "used by Donkey Kong 3 only", so on this set it is normally 0 and
        // the wrap is a no-op -- kept because relying on that is an
        // assumption, and the mask costs nothing.
        int code  = ((sys->sprite_ram[offs + 1] & 0x7F) +
                     ((sys->sprite_ram[offs + 2] & 0x40) << 1)) & 0x7F;
        uint8_t color = (uint8_t)((sys->sprite_ram[offs + 2] & 0x0F) + 16 * sys->palette_bank);
        bool flipx = (sys->sprite_ram[offs + 2] & 0x80) != 0;
        bool flipy = (sys->sprite_ram[offs + 1] & 0x80) != 0;

        int x = (sys->sprite_ram[offs + 3] + add_x + 1) & 0xFF;
        if (flip) { x = (x ^ 0xFF) - 15; flipx = !flipx; }

        int y_top = scanline - ((y + add_y + 1 + scanline_vfc) & 0x0F);
        int local_y = scanline - y_top;
        if (local_y < 0 || local_y >= 16) { num_sprt++; continue; }
        int sy = flipy ? (15 - local_y) : local_y;

        // "On real hardware, sprites wrap around from the right to the left
        // instead of clipping" -- draw_sprites(). The wraparound copy is
        // drawn unconditionally; it only lands on screen near an edge.
        for (int copy = 0; copy < 2; copy++) {
            int sx0 = (copy == 0) ? x : (flip ? x + 256 : x - 256);
            for (int cx = 0; cx < 16; cx++) {
                int ox = sx0 + cx;
                if (ox < 0 || ox >= DKONG_GAME_WIDTH) continue;
                int px = flipx ? (15 - cx) : cx;
                uint8_t pixel = sprite_pixels[code][px][sy];
                if (pixel == 0) continue; // transpen 0
                out[ox] = pen_color(color, pixel);
            }
        }
        num_sprt++;
    }

    if ((uint32_t)num_sprt > g_sprite_peak) g_sprite_peak = (uint32_t)num_sprt;
    if (num_sprt >= 16) g_sprite_limit_hits++;
}

#if defined(DKONG_COST_TRACE) && (defined(ARDUINO_ARCH_RP2040) || defined(PICO_ON_DEVICE))
#include <Arduino.h> // micros()
#define DK_COST_NOW() micros()
#define DK_COST_ADD(acc, t0) do { (acc) += micros() - (t0); } while (0)
#define DK_COUNT(c) do { (c)++; } while (0)
#else
#define DK_COST_NOW() 0u
#define DK_COST_ADD(acc, t0) do { (void)(t0); } while (0)
#define DK_COUNT(c) do { } while (0)
#endif

// Landscape's two costs, added when the column renderer replaced the
// whole-frame burst and landscape turned from a BURST problem into a
// THROUGHPUT one (DEVNOTES #92/#93): the once-per-frame sprite arbitration
// walk, and the per-scanline column render.
static uint32_t g_begin_us = 0; // dkong_video_begin_frame()
static uint32_t g_cols_us  = 0; // render_native_column()
static uint32_t g_cols_n   = 0;

// COLUMN RENDERING -- what replaced the 114KB full-frame cache.
//
// Landscape needs a raster COLUMN per canvas scanline, and the obvious way
// to get one is to render every row into a cache first. That is what this
// game did, and it is why both landscape rotations were roughly 3/4 red on
// a real screen: `run_frame_sequential()` ran a whole frame of CPU and then
// rendered all 224 rows before submitting a single scanline. The 32-buffer
// queue is 2,032us of runway (DEVNOTES #85) and that burst is many
// milliseconds, so once the runway was spent the rest of the frame starved.
//
// The other four games were converted to a column primitive in phase 3;
// Donkey Kong was left because of the sprite arbitration below.
//
// THE ARBITRATION PROBLEM. Real hardware evaluates sprites against a 64x9
// line buffer and stops after 16 per scanline, and the game relies on it.
// That limit is decided by walking sprite RAM IN ORDER for one scanline, so
// a column renderer -- which visits a fixed x down every y -- cannot decide
// it independently per pixel.
//
// THE PROPERTY THAT MAKES IT WORK. The coarse test is
// ((y + add_y + 1 + scanline_vf) & 0xF0) == 0xF0. Call that sum t: it grows
// by exactly 1 per scanline, so the test passes for exactly 16 CONSECUTIVE
// scanlines, and local_y = t & 0x0F runs 0..15 across them in order. A
// sprite is therefore always a contiguous 16-line band; arbitration can only
// DROP individual lines inside it, never move or split it. (16 lines out of
// every 256 also means at most one band can fall in the 224 visible lines,
// so there is no second band to worry about.)
//
// THE DIRECTION TRAP, found by the self-test at the bottom of this file and
// not by eye. With flip_screen set, scanline_vfc is XORed with 0xFF, so the
// sum DECREASES by one per scanline and local_y runs 15 -> 0 down the raster
// instead of 0 -> 15. The band is still 16 contiguous lines, but the row a
// given sprite line lands on is y0 - local_y rather than y0 + local_y. Miss
// that and the picture is right in every unflipped frame and silently
// scrambled the moment the game flips the screen -- which it does for
// player 2. Hence `ydown`.
//
// So the arbitration is run once per frame, exactly as the row renderer runs
// it, and each sprite is recorded as {top line, 16-bit mask of the lines it
// actually drew on}. Those are then bucketed by column, so a column visits
// only the two or three sprites that overlap it instead of all 128.
//
// Cost: the per-frame arbitration walk is the SAME work the row path already
// did per scanline, just hoisted; the column pass is ~3 sprites x 16 rows
// instead of 224 rows x 16 sprites. And it frees 114KB of SRAM.
#define DK_MAX_INST     255u   // sprite instances per frame (128 sprites x 2 wrap copies)
#define DK_MAX_PER_COL   64u   // instances overlapping one raster column

typedef struct {
    int16_t  x;        // leftmost raster column of this copy (may be negative)
    int16_t  y0;       // raster row carrying the band's local_y == 0 line
    uint16_t mask;     // which of the 16 band lines survived arbitration
    uint8_t  code, color, flipx, flipy;
    uint8_t  ydown;    // 1 when raster row DECREASES as local_y increases
} dk_spr_inst;

static dk_spr_inst g_inst[DK_MAX_INST];
static uint8_t     g_inst_n;
static uint8_t     g_col_n[DKONG_GAME_WIDTH];
static uint8_t     g_col_i[DKONG_GAME_WIDTH][DK_MAX_PER_COL];

// Latches this frame's sprites for the column renderer. Must run before the
// first render_native_column() of a frame; harmless (just unused) in tate,
// which keeps its own per-scanline arbitration inside render_native_row().
void dkong_video_begin_frame(const dkong_system *sys) {
    const uint32_t t_begin = DK_COST_NOW();
    g_inst_n = 0;
    for (uint32_t c = 0; c < (uint32_t)DKONG_GAME_WIDTH; c++) g_col_n[c] = 0;

    const bool flip = sys->flip_screen;
    const int add_y = flip ? 0xF7 : 0xF9;
    const int add_x = 0xF7;
    const int base  = sys->sprite_bank << 9;

    // The band, computed rather than searched for. The old version asked
    // "which sprites are on this line?" for all 224 lines, which scans all
    // 128 sprite slots on every line (28,672 iterations) because the loop
    // only exits early once 16 sprites have passed. Measured on hardware:
    // 1,414us a frame, which is most of why landscape stopped being a burst
    // problem and became a throughput one (DEVNOTES #93).
    //
    // Inverted, it is 128 sprites x 16 lines = 2,048. The coarse test is
    // ((y + add_y + 1 + scanline_vf) & 0xF0) == 0xF0, and scanline_vf is
    // just (ny + VBEND - 1) & 0xFF -- so the sum is (A + ny) & 0xFF with A
    // constant per sprite, and the band is simply the 16 rows starting where
    // that sum hits 0xF0. Under flip the sum RUNS BACKWARDS (see THE
    // DIRECTION TRAP), so it is (B - ny) instead and the rows descend.
    //
    // y0 may be negative or past the bottom: a band whose start is off the
    // visible window can still have its tail on screen, so it is kept as a
    // signed row and each line is range-checked rather than clamping y0.
    static uint8_t s_count[DKONG_GAME_HEIGHT]; // sprites already taken per line
    for (uint32_t i = 0; i < (uint32_t)DKONG_GAME_HEIGHT; i++) s_count[i] = 0;
    static int16_t  s_ytop[128];
    static uint16_t s_mask[128];
    for (uint32_t i = 0; i < 128u; i++) s_mask[i] = 0;

    for (int slot = 0; slot < 128; slot++) {
        const int offs = base + slot * 4;
        const int y = sys->sprite_ram[offs];

        int y0;
        if (!flip) {
            // sum = (y + add_y + 1 + (ny + VBEND - 1)) & 0xFF
            const uint32_t A = (uint32_t)(y + add_y + (int)DKONG_VBEND) & 0xFFu;
            y0 = (int)((0xF0u - A) & 0xFFu);
        } else {
            // sum = (y + add_y + 1 + (255 - (ny + VBEND - 1))) & 0xFF
            const uint32_t B = (uint32_t)(y + add_y + 257 - (int)DKONG_VBEND) & 0xFFu;
            y0 = (int)((B - 0xF0u) & 0xFFu);
        }
        // Wraparound, and it is NOT symmetric. Unflipped the band ASCENDS
        // from y0, so a y0 past the bottom can still have its tail on screen
        // via the mod-256 wrap (y0=250 really means rows 0..9 carrying
        // local_y 6..15) -- hence the shift to a negative start. Flipped the
        // band DESCENDS from y0, so a high y0 already reaches down into the
        // visible rows on its own and shifting it would throw the band away.
        // The first version shifted in both directions and silently dropped
        // flipped sprites near the bottom of the screen; the self-test found
        // it at x=97 y=215.
        if (!flip && y0 > (int)DKONG_GAME_HEIGHT - 1) y0 -= 256;

        uint16_t mask = 0;
        for (int ly = 0; ly < 16; ly++) {
            const int ny = flip ? (y0 - ly) : (y0 + ly);
            if (ny < 0 || ny >= DKONG_GAME_HEIGHT) continue;
            // Arbitration: real hardware takes the first 16 sprites in
            // sprite-RAM order on each line and drops the rest. Walking
            // slots in order and counting per line is exactly that.
            if (s_count[ny] >= 16) continue;
            s_count[ny]++;
            mask |= (uint16_t)(1u << ly);
        }
        if (!mask) continue;
        s_ytop[slot] = (int16_t)y0;
        s_mask[slot] = mask;
    }

    // Turn the surviving bands into instances, in sprite-RAM order so a later
    // sprite still paints over an earlier one exactly as the row path does.
    for (int slot = 0; slot < 128; slot++) {
        if (s_mask[slot] == 0) continue;
        const int offs = base + slot * 4;

        const int code = ((sys->sprite_ram[offs + 1] & 0x7F) +
                          ((sys->sprite_ram[offs + 2] & 0x40) << 1)) & 0x7F;
        const uint8_t color = (uint8_t)((sys->sprite_ram[offs + 2] & 0x0F) + 16 * sys->palette_bank);
        bool flipx = (sys->sprite_ram[offs + 2] & 0x80) != 0;
        const bool flipy = (sys->sprite_ram[offs + 1] & 0x80) != 0;

        int x = (sys->sprite_ram[offs + 3] + add_x + 1) & 0xFF;
        if (flip) { x = (x ^ 0xFF) - 15; flipx = !flipx; }

        // Both wraparound copies, same as the row path: "sprites wrap around
        // from the right to the left instead of clipping".
        for (int copy = 0; copy < 2; copy++) {
            const int sx0 = (copy == 0) ? x : (flip ? x + 256 : x - 256);
            if (sx0 >= DKONG_GAME_WIDTH || sx0 + 16 <= 0) continue;
            if (g_inst_n >= DK_MAX_INST) break;

            dk_spr_inst *e = &g_inst[g_inst_n];
            e->x = (int16_t)sx0;  e->y0 = s_ytop[slot];  e->mask = s_mask[slot];
            e->code = (uint8_t)code; e->color = color;
            e->flipx = flipx ? 1u : 0u; e->flipy = flipy ? 1u : 0u;
            e->ydown = flip ? 1u : 0u;

            int c0 = sx0 < 0 ? 0 : sx0;
            int c1 = sx0 + 16 > DKONG_GAME_WIDTH ? DKONG_GAME_WIDTH : sx0 + 16;
            for (int c = c0; c < c1; c++)
                if (g_col_n[c] < DK_MAX_PER_COL) g_col_i[c][g_col_n[c]++] = g_inst_n;
            g_inst_n++;
        }
    }
    DK_COST_ADD(g_begin_us, t_begin);
}

// One raster COLUMN: out[ny] for ny in [0, DKONG_GAME_HEIGHT). The transpose
// of render_native_row(), and it must agree with it pixel for pixel.
static void render_native_column(const dkong_system *sys, uint32_t native_x, uint16_t *out) {
    const bool flip = sys->flip_screen;
    const uint32_t tcol = flip ? (31u - (native_x >> 3)) : (native_x >> 3);
    const uint32_t tx   = flip ? (7u - (native_x & 7u)) : (native_x & 7u);

    // Background tilemap, walked one TILE ROW at a time. Per pixel this used
    // to recompute the tile index, its code, its PROM colour (two divisions)
    // and re-test whether the tile row had changed: 23.3us a column, ~26
    // cycles a pixel, 5,600us a frame (DEVNOTES #93). Everything except the
    // pixel fetch is constant across a tile row, so it is hoisted and the
    // inner loop is a palette lookup and a store.
    //
    // Under flip the raster row and the tile's y run in OPPOSITE directions
    // -- ty = 255 - screen_y -- so py counts DOWN. Same trap as the sprite
    // bands, and the reason the two directions are separate loops rather
    // than a ternary inside one.
    const uint8_t pbank = (uint8_t)(0x10 * sys->palette_bank);
    uint32_t ny = 0;
    while (ny < (uint32_t)DKONG_GAME_HEIGHT) {
        const uint32_t screen_y = ny + DKONG_VBEND;
        const uint32_t ty   = flip ? (255u - screen_y) : screen_y;
        const uint32_t trow = ty >> 3;
        uint32_t py = ty & 7u;

        const uint16_t tile_index = (uint16_t)(trow * 32u + tcol);
        const uint8_t  code  = sys->video_ram[tile_index];
        const uint8_t  color = (uint8_t)((dkong_color_prom[(tile_index % 32u) + 32u * (tile_index / 32u / 4u)] & 0x0F)
                                         + pbank);
        const uint16_t *pen = &rgb565_palette[(uint32_t)(color & 0x3Fu) << 2];
        const uint8_t  *tp  = tile_pixels[code][tx];

        if (!flip) {
            while (py < 8u && ny < (uint32_t)DKONG_GAME_HEIGHT)
                out[ny++] = pen[tp[py++] & 3u];
        } else {
            for (;;) {
                out[ny++] = pen[tp[py] & 3u];
                if (py == 0u || ny >= (uint32_t)DKONG_GAME_HEIGHT) break;
                py--;
            }
        }
    }

    // Sprites: only the handful bucketed onto this column, in draw order.
    const uint8_t n = g_col_n[native_x];
    const uint8_t *idx = g_col_i[native_x];
    for (uint8_t k = 0; k < n; k++) {
        const dk_spr_inst *e = &g_inst[idx[k]];
        const int cx  = (int)native_x - e->x;         // 0..15 by construction
        const int spx = e->flipx ? (15 - cx) : cx;
        uint16_t m = e->mask;
        for (int ly = 0; ly < 16; ly++) {
            if (!(m & (uint16_t)(1u << ly))) continue;
            const int ny = e->ydown ? (e->y0 - ly) : (e->y0 + ly);
            if (ny < 0 || ny >= DKONG_GAME_HEIGHT) continue;
            const int sy = e->flipy ? (15 - ly) : ly;
            const uint8_t pixel = sprite_pixels[e->code][spx][sy];
            if (pixel == 0) continue; // transpen 0
            out[ny] = pen_color(e->color, pixel);
        }
    }
}

// Frame-cost instrument. This game is the project's tightest budget and,
// unlike Burger Time (#59), had no way to see WHERE its frame goes -- which
// left the aspect correction's cost a matter of argument rather than
// measurement, and sent the first attempt to optimise it at the wrong loop
// (#78). Same shape as btime_machine.cpp's COST_NOW/COST_ADD, guarded so
// the host harness compiles unchanged.
// OPT-IN, because the instrument perturbs what it measures. Four micros()
// calls per scanline is only ~0.15ms a frame, but `starve` is sensitive to
// intra-frame unevenness rather than to totals (#35): switching this on took
// Donkey Kong's unstretched build from starve 0/60 to 2393-4014/60 while
// `work` barely moved. Build with
//   --build-property compiler.cpp.extra_flags=-DDKONG_COST_TRACE=1
// when you want the split, and read the numbers knowing they cost something.
static uint32_t g_rows_us  = 0; // time inside render_native_row()
static uint32_t g_emit_us  = 0; // time turning that row into canvas pixels
static uint32_t g_rows_n   = 0; // render_native_row() calls (memoisation check)
static uint32_t g_lines_n  = 0; // scanlines that did any work at all

void dkong_debug_take_landscape(uint32_t *begin_us, uint32_t *cols_us, uint32_t *cols_n) {
    *begin_us = g_begin_us; *cols_us = g_cols_us; *cols_n = g_cols_n;
    g_begin_us = g_cols_us = g_cols_n = 0;
}

void dkong_debug_take_render(uint32_t *rows_us, uint32_t *emit_us,
                             uint32_t *rows, uint32_t *lines) {
    *rows_us = g_rows_us; *emit_us = g_emit_us;
    *rows = g_rows_n;     *lines = g_lines_n;
    g_rows_us = g_emit_us = g_rows_n = g_lines_n = 0;
}

void dkong_video_render_scanline(const dkong_system *sys, uint32_t dvi_y, uint16_t *buf) {
    memset(buf, 0, HAL_VIDEO_WIDTH * sizeof(uint16_t));
    bool mir = sys->mirror_x;
    static uint16_t row[DKONG_GAME_WIDTH]; // scratch for tate's on-demand row

    switch (sys->rotation) {

    case 0: {
        // Landscape. `av_yoko.row` maps this canvas row onto the raster's
        // LONG axis; `av_yoko.col` maps canvas columns onto the SHORT axis
        // -- and in yoko the SHORT axis is the one that must NARROW (180
        // canvas columns, not DKONG_GAME_HEIGHT's worth at 1:1). See
        // arcade_video_geom.h.
        //
        // The `(W-1) - dy` reversal is NOT cosmetic: a 90-degree rotation
        // must reverse exactly one axis relative to tate (case 1, which
        // does not reverse its equivalent), or the result is a mirror
        // image rather than a rotation (DEVNOTES #21).
        const uint32_t dy  = av_yoko.row[dvi_y];
        const uint32_t col = (uint32_t)(DKONG_GAME_WIDTH - 1) - dy;
        // Rendered on demand, one raster column per canvas scanline -- see
        // render_native_column(). This is what removed the whole-frame burst
        // that made both landscape rotations 3/4 red.
        static uint16_t colbuf[DKONG_GAME_HEIGHT];
        const uint32_t t_col = DK_COST_NOW();
        render_native_column(sys, col, colbuf);
        DK_COST_ADD(g_cols_us, t_col);
        DK_COUNT(g_cols_n);
        if (av_yoko.col_1to1) {
            uint16_t *out = buf + av_yoko.x0;
            for (uint32_t i = 0; i < (uint32_t)DKONG_GAME_HEIGHT; i++) {
                const uint32_t dx = mir ? (uint32_t)(DKONG_GAME_HEIGHT - 1) - i : i;
                out[i] = colbuf[dx];
            }
        } else {
            for (uint32_t x = av_yoko.x0; x < av_yoko.x1; x++) {
                const uint32_t i  = av_yoko.col[x];
                const uint32_t dx = mir ? (uint32_t)(DKONG_GAME_HEIGHT - 1) - i : i;
                buf[x] = colbuf[dx];
            }
        }
        break;
    }

    case 1: {
        // 90 deg CCW (tate). Rendered on demand, one raster row per call --
        // tate renders its one needed row directly; only landscape needs
        // the column primitive and its per-frame sprite latch.
        if (dvi_y < av_tate.y0 || dvi_y >= av_tate.y1) return;
        uint32_t dx = av_tate.row[dvi_y];
        if (mir) dx = (uint32_t)(DKONG_GAME_HEIGHT - 1) - dx;
        // Skip the render when this canvas row repeats the previous one.
        // The aspect correction upsamples 224 raster rows onto 240 canvas
        // rows, so 16 of them are duplicates, and duplicates are adjacent --
        // 16 of 240 render_native_row() calls, ~7% of the renderer. Costs
        // one compare per scanline when the map is 1:1 and nothing repeats.
        //
        // The duplicate then shows raster state from an instant earlier in
        // the frame's CPU execution than a re-render would. That is the same
        // kind of intra-frame staleness the interleaved renderer already has
        // by design, at 1/240th of a frame.
        static uint32_t last_dx = 0xFFFFFFFFu;
        if (dvi_y == av_tate.y0) last_dx = 0xFFFFFFFFu; // new frame
        DK_COUNT(g_lines_n);
        if (dx != last_dx) {
            const uint32_t t0 = DK_COST_NOW();
            render_native_row(sys, dx, row);
            DK_COST_ADD(g_rows_us, t0);
            DK_COUNT(g_rows_n);
            last_dx = dx;
        }
        const uint32_t te = DK_COST_NOW();
        // The 1:1 branch is a MEASURED requirement, not tidiness: going
        // through av_tate.col[] unconditionally cost this family +1.6ms a
        // frame and put Donkey Kong's work_max past the budget. See
        // av_map_t::col_1to1.
        if (av_tate.col_1to1) {
            uint16_t *out = buf + av_tate.x0;
            for (uint32_t c = 0; c < (uint32_t)DKONG_GAME_WIDTH; c++) out[c] = row[c];
        } else {
            av_emit_row_wide(buf, row, &av_tate);
        }
        DK_COST_ADD(g_emit_us, te);
        break;
    }

    case 2: {
        // 180 deg: case 0 with BOTH axes reversed relative to it (a 180 is
        // two 90s), so `col` is deliberately the *un*-reversed raw value
        // (case 0 already reversed it once; reversing that again would
        // cancel out) and `dx`'s ternary is the mirror of case 0's.
        const uint32_t col = av_yoko.row[dvi_y];
        // Rendered on demand, one raster column per canvas scanline -- see
        // render_native_column(). This is what removed the whole-frame burst
        // that made both landscape rotations 3/4 red.
        static uint16_t colbuf[DKONG_GAME_HEIGHT];
        const uint32_t t_col = DK_COST_NOW();
        render_native_column(sys, col, colbuf);
        DK_COST_ADD(g_cols_us, t_col);
        DK_COUNT(g_cols_n);
        if (av_yoko.col_1to1) {
            uint16_t *out = buf + av_yoko.x0;
            for (uint32_t i = 0; i < (uint32_t)DKONG_GAME_HEIGHT; i++) {
                const uint32_t dx = mir ? i : (uint32_t)(DKONG_GAME_HEIGHT - 1) - i;
                out[i] = colbuf[dx];
            }
        } else {
            for (uint32_t x = av_yoko.x0; x < av_yoko.x1; x++) {
                const uint32_t i  = av_yoko.col[x];
                const uint32_t dx = mir ? i : (uint32_t)(DKONG_GAME_HEIGHT - 1) - i;
                buf[x] = colbuf[dx];
            }
        }
        break;
    }

    case 3: {
        // 90 deg CW (tate) -- case 1 with both the scanline order and the
        // within-scanline order reversed.
        if (dvi_y < av_tate.y0 || dvi_y >= av_tate.y1) return;
        const uint32_t d  = av_tate.row[dvi_y];
        const uint32_t dx = mir ? d : (uint32_t)(DKONG_GAME_HEIGHT - 1) - d;
        // Skip the render when this canvas row repeats the previous one.
        // The aspect correction upsamples 224 raster rows onto 240 canvas
        // rows, so 16 of them are duplicates, and duplicates are adjacent --
        // 16 of 240 render_native_row() calls, ~7% of the renderer. Costs
        // one compare per scanline when the map is 1:1 and nothing repeats.
        //
        // The duplicate then shows raster state from an instant earlier in
        // the frame's CPU execution than a re-render would. That is the same
        // kind of intra-frame staleness the interleaved renderer already has
        // by design, at 1/240th of a frame.
        static uint32_t last_dx = 0xFFFFFFFFu;
        if (dvi_y == av_tate.y0) last_dx = 0xFFFFFFFFu; // new frame
        if (dx != last_dx) { render_native_row(sys, dx, row); last_dx = dx; }
        if (av_tate.col_1to1) {
            uint16_t *out = buf + av_tate.x0;
            for (uint32_t c = 0; c < (uint32_t)DKONG_GAME_WIDTH; c++)
                out[c] = row[(uint32_t)(DKONG_GAME_WIDTH - 1u) - c];
        } else {
            av_emit_row_wide_rev(buf, row, &av_tate);
        }
        break;
    }

    default: break;
    }
}

// SELF-TEST: the column path must reproduce the row path exactly.
//
// Host-only (it needs a 114KB reference buffer -- the very cache this work
// deleted from the device). This is the check that needs no historical
// reference image and no hardware: render the native raster BOTH ways and
// compare. If the sprite arbitration, the wraparound copies, the flip
// handling or the tile lookup differ by one pixel between the two, this
// says so and says where.
//
// Worth having because the two paths cannot be compared by eye: they are
// used in different rotations, so a divergence shows up as "landscape looks
// subtly wrong" long after the change that caused it.
#if defined(DKONG_HOST_SELFTEST)
#include <stdio.h>
int dkong_video_selftest_column_vs_row(const dkong_system *sys) {
    static uint16_t by_row[DKONG_GAME_HEIGHT][DKONG_GAME_WIDTH];
    static uint16_t colbuf[DKONG_GAME_HEIGHT];
    int bad = 0;

    for (uint32_t y = 0; y < (uint32_t)DKONG_GAME_HEIGHT; y++)
        render_native_row(sys, y, by_row[y]);

    dkong_video_begin_frame(sys);
    for (uint32_t x = 0; x < (uint32_t)DKONG_GAME_WIDTH; x++) {
        render_native_column(sys, x, colbuf);
        for (uint32_t y = 0; y < (uint32_t)DKONG_GAME_HEIGHT; y++) {
            if (colbuf[y] != by_row[y][x]) {
                // On a failure, dump the column's instance list -- that is
                // what identified the flip-direction bug (THE DIRECTION
                // TRAP above) in one run rather than by bisecting frames.
                if (bad == 0) {
                    printf("  MISMATCH x=%u y=%u  column=%04x row=%04x\n",
                           (unsigned)x, (unsigned)y, colbuf[y], by_row[y][x]);
                    printf("  sprite_bank=%u palette_bank=%u flip=%d, %u inst on this column\n",
                           (unsigned)sys->sprite_bank, (unsigned)sys->palette_bank,
                           (int)sys->flip_screen, (unsigned)g_col_n[x]);
                    for (uint8_t k = 0; k < g_col_n[x]; k++) {
                        const dk_spr_inst *e = &g_inst[g_col_i[x][k]];
                        printf("    inst %u: x=%d y0=%d mask=%04x code=%02x color=%02x "
                               "fx=%u fy=%u ydown=%u\n",
                               (unsigned)g_col_i[x][k], (int)e->x, (int)e->y0,
                               (unsigned)e->mask, (unsigned)e->code, (unsigned)e->color,
                               (unsigned)e->flipx, (unsigned)e->flipy, (unsigned)e->ydown);
                    }
                }
                bad++;
            }
        }
    }
    return bad;
}
#endif

void dkong_draw_frame(dkong_system *system) {
    if (system->rotation == 0 || system->rotation == 2)
        dkong_video_begin_frame(system);

    for (uint32_t i = 0; i < HAL_VIDEO_HEIGHT; i++) {
        uint16_t *buf = hal_video_acquire_scanline();
        dkong_video_render_scanline(system, i, buf);
        hal_video_submit_scanline(buf);
    }
}

void dkong_draw_error_frame(uint16_t color) {
    for (uint32_t i = 0; i < HAL_VIDEO_HEIGHT; i++) {
        uint16_t *buf = hal_video_acquire_scanline();
        for (uint32_t x = 0; x < HAL_VIDEO_WIDTH; x++) buf[x] = color;
        hal_video_submit_scanline(buf);
    }
}
