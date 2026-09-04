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

// Full native frame cache, used ONLY by landscape/180 rotation, which
// structurally need every native row before they can emit one physical
// scanline (a column slice reads all 224 rows at once). Tate/CW render
// their one needed row on demand instead. Same arrangement, and the same
// known stall risk for the two yoko modes, as ArcadeMachine_Pacman -- see
// DEVNOTES.md problems #18/#19/#20.
static uint16_t frame_cache[DKONG_GAME_HEIGHT][DKONG_GAME_WIDTH];

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

static uint32_t g_rows_us  = 0; // time inside render_native_row()
static uint32_t g_emit_us  = 0; // time turning that row into canvas pixels
static uint32_t g_rows_n   = 0; // render_native_row() calls (memoisation check)
static uint32_t g_lines_n  = 0; // scanlines that did any work at all

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
        if (av_yoko.col_1to1) {
            uint16_t *out = buf + av_yoko.x0;
            for (uint32_t i = 0; i < (uint32_t)DKONG_GAME_HEIGHT; i++) {
                const uint32_t dx = mir ? (uint32_t)(DKONG_GAME_HEIGHT - 1) - i : i;
                out[i] = frame_cache[dx][col];
            }
        } else {
            for (uint32_t x = av_yoko.x0; x < av_yoko.x1; x++) {
                const uint32_t i  = av_yoko.col[x];
                const uint32_t dx = mir ? (uint32_t)(DKONG_GAME_HEIGHT - 1) - i : i;
                buf[x] = frame_cache[dx][col];
            }
        }
        break;
    }

    case 1: {
        // 90 deg CCW (tate). Rendered on demand, one raster row per call --
        // see frame_cache's header comment for why tate must NOT read it.
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
            av_emit_row(buf, row, &av_tate);
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
        if (av_yoko.col_1to1) {
            uint16_t *out = buf + av_yoko.x0;
            for (uint32_t i = 0; i < (uint32_t)DKONG_GAME_HEIGHT; i++) {
                const uint32_t dx = mir ? i : (uint32_t)(DKONG_GAME_HEIGHT - 1) - i;
                out[i] = frame_cache[dx][col];
            }
        } else {
            for (uint32_t x = av_yoko.x0; x < av_yoko.x1; x++) {
                const uint32_t i  = av_yoko.col[x];
                const uint32_t dx = mir ? i : (uint32_t)(DKONG_GAME_HEIGHT - 1) - i;
                buf[x] = frame_cache[dx][col];
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
            av_emit_row_rev(buf, row, &av_tate);
        }
        break;
    }

    default: break;
    }
}

void dkong_draw_frame(dkong_system *system) {
    if (system->rotation == 0 || system->rotation == 2) {
        for (uint32_t y = 0; y < (uint32_t)DKONG_GAME_HEIGHT; y++)
            render_native_row(system, y, frame_cache[y]);
    }

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
