// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Pac-Man tile+sprite renderer.
//
// Every hardware fact below was verified against MAME's pacman_v.cpp /
// pacman.cpp AND src/emu/drawgfx.cpp (upstream mamedev/mame), not inferred:
//
// - Tile format (tilelayout) and sprite format (spritelayout): both 2bpp,
//   4 pixels packed per byte. Two non-obvious conventions from
//   drawgfx.cpp's gfx_element::decode() -- gotten wrong in an earlier
//   version of this file and corrected after real-hardware testing showed
//   scrambled sprites ("Pac-Man looks like a tennis ball") -- apply on top
//   of the planeoffset/xoffset/yoffset bit-address tables in pacman.cpp:
//     1. `readbit()` is MSB-first (`src[addr/8] & (0x80 >> (addr%8))`),
//        i.e. bit-within-byte position is `7 - (addr%8)`, not `addr%8`.
//     2. Plane 0 contributes the HIGH bit of the pixel value and plane 1
//        the LOW bit (`planebit` starts at `1 << (planes-1)` for plane 0
//        and shifts down), i.e. pixel = (plane0<<1) | plane1 -- backwards
//        from the naive "plane N contributes bit N" assumption.
//   Combined, and cross-checked pixel-by-pixel against these exact tables
//   in Python before porting to C: within each byte, the HIGH nibble holds
//   plane 0's bit per pixel and the LOW nibble holds plane 1's, and a
//   pixel's 4-pixel-group-local column position (0-3) maps to nibble bit
//   position `3 - local` (i.e. local=0 reads each nibble's own MSB, local=3
//   reads each nibble's own LSB) -- see pixel2_from_byte() below.
// - Video RAM addressing (pacman_scan_rows tilemap mapper): the 36x28
//   logical tilemap does NOT sit row-major in video_ram/color_ram -- the
//   leftmost/rightmost 2 tile columns (the score/lives/level side panels)
//   are addressed differently from the 32-column playfield. Ported
//   verbatim from pacman_state::pacman_scan_rows()'s formula.
// - Sprite positioning (draw_sprites): sx = 272 - spriteram2[x], sy =
//   spriteram2[y] - 31, drawn twice (once more at sx-256) for tunnel
//   wraparound, sprites 0-2 (byte offsets 0/2/4) get a +1 Y "xoffsethack"
//   real Pac-Man/Puckman boards need for correct placement, and all 8
//   sprites draw in descending index order (7..0, so sprite 0 is topmost).
// - Palette PROM (82s123.7f) decode: a resistor-ladder DAC (1000/470/220
//   ohm for R/G, 470/220 ohm for B) -- ported from pacman_palette()'s
//   compute_resistor_weights() call. The weight constants below were
//   computed from those exact resistances (255 * (1/R_i) / sum(1/R_j))
//   and verified by hand against the real pacman_assets/rom/82s123.7f
//   bytes -- e.g. byte 0x07 (pen 1, Blinky's red) decodes to (255,0,0),
//   byte 0xC9 (pen 11, the maze's iconic blue) decodes to (33,33,255).
// - Color lookup PROM (82s126.4a): pen = lookup[attr*4 + pixel2bit] &
//   0x0F, where attr is colorram/spriteram's low 5 bits (colortablebank/
//   palettebank are always 0 on this hardware -- those exist for sibling
//   games sharing this video chip, e.g. Pengo).
#include <string.h>
#include "pacman_video.h"
#include "arcade_hal_video.h"
#include "arcade_video_geom.h"

uint8_t pacman_gfx_rom[PACMAN_GFX_ROM_SIZE];
uint8_t pacman_palette_prom[PACMAN_PALETTE_PROM_SIZE];
uint8_t pacman_lookup_prom[PACMAN_LOOKUP_PROM_SIZE];

// Decoded caches, built once by pacman_video_build_caches().
static uint8_t tile_pixels[256][8][8];   // [tile][x][y] -> 2bpp pixel (0-3)
static uint8_t sprite_pixels[64][16][16]; // [sprite][x][y] -> 2bpp pixel (0-3)
static uint16_t rgb565_palette[32];

// Where the picture lands on the canvas, and how it is resampled to get
// there, now comes from ArcadeHAL's arcade_video_geom.h -- av_tate and
// av_yoko, built once by av_geom_init(PACMAN_GAME_WIDTH,
// PACMAN_GAME_HEIGHT) in pacman_init(). The per-file TATE_BX/TATE_BY/
// LAND_BX constants this used to carry are gone: they were four
// hand-derived cases per game, and deriving one from a sibling rather than
// from a single source of truth is what produced DEVNOTES #21, #23 and #33.
//
// Read arcade_video_geom.h before changing anything below. In particular,
// the picture's DESTINATION is the same for every game in the project --
// 320x240 in tate, 180x240 in yoko -- and only the resampling ratio is
// Pac-Man-specific.

// Maps logical tilemap (col 0-35, row 0-27) to its video_ram/color_ram
// byte offset. Ported verbatim from pacman_state::pacman_scan_rows() --
// do not "simplify" this to row*36+col, the real hardware genuinely
// doesn't address memory that way (see this file's header comment).
static inline uint16_t scan_rows(uint32_t col, uint32_t row) {
    row = (row + 2) & 0xFFu;      // tilemap offsets fit in 10 bits total;
    col = (col - 2) & 0xFFu;      // masking width doesn't matter as long as
                                   // it's wide enough to preserve the
                                   // underflow-wraps-high-bit behavior the
                                   // original uint32_t arithmetic relies on.
    if (col & 0x20u)
        return (uint16_t)(row + ((col & 0x1Fu) << 5));
    else
        return (uint16_t)(col + (row << 5));
}

// Extracts the 2bpp pixel value for 4-pixel-group-local column `local`
// (0-3) from one packed byte -- see this file's header comment for the
// MSB-first/plane-order derivation. Shared by both the tile and sprite
// decode loops below, since both formats pack pixels into bytes the same
// way (only which byte holds which (x,y) differs between the two).
static inline uint8_t pixel2_from_byte(uint8_t byte, int local) {
    int nib_shift = 3 - local;
    uint8_t plane0 = (byte >> (4 + nib_shift)) & 1u; // high nibble -> high pixel bit
    uint8_t plane1 = (byte >> nib_shift) & 1u;       // low nibble  -> low pixel bit
    return (uint8_t)((plane0 << 1) | plane1);
}

void pacman_video_build_caches(void) {
    // --- Tiles: gfx1 offset 0x0000, 16 bytes/tile, 256 tiles. ---
    for (int t = 0; t < 256; t++) {
        const uint8_t *base = &pacman_gfx_rom[t * 16];
        for (int y = 0; y < 8; y++) {
            uint8_t byte_x4_7 = base[y];     // columns 4-7 of this row
            uint8_t byte_x0_3 = base[8 + y]; // columns 0-3 of this row
            for (int local = 0; local < 4; local++) {
                tile_pixels[t][local][y]     = pixel2_from_byte(byte_x0_3, local);
                tile_pixels[t][4 + local][y] = pixel2_from_byte(byte_x4_7, local);
            }
        }
    }

    // --- Sprites: gfx1 offset 0x1000, 64 bytes/sprite, 64 sprites. ---
    for (int s = 0; s < 64; s++) {
        const uint8_t *base = &pacman_gfx_rom[0x1000 + s * 64];
        for (int y = 0; y < 16; y++) {
            int y_byte = (y < 8) ? y : (y + 24);
            for (int xg = 0; xg < 4; xg++) {
                int x_base;
                switch (xg) {
                case 0: x_base = 8;  break; // columns 0-3
                case 1: x_base = 16; break; // columns 4-7
                case 2: x_base = 24; break; // columns 8-11
                default: x_base = 0; break; // columns 12-15
                }
                uint8_t byte = base[x_base + y_byte];
                for (int local = 0; local < 4; local++) {
                    int x = xg * 4 + local;
                    sprite_pixels[s][x][y] = pixel2_from_byte(byte, local);
                }
            }
        }
    }

    // --- Palette: 82s123.7f, resistor-ladder DAC. ---
    static const double RGWEIGHT[3] = { 33.232922, 70.708344, 151.058735 }; // 1000/470/220 ohm
    static const double BWEIGHT[2]  = { 81.304348, 173.695652 };           // 470/220 ohm
    for (int i = 0; i < 32; i++) {
        uint8_t b = pacman_palette_prom[i];
        int r = (int)(RGWEIGHT[0] * ((b >> 0) & 1) + RGWEIGHT[1] * ((b >> 1) & 1) + RGWEIGHT[2] * ((b >> 2) & 1) + 0.5);
        int g = (int)(RGWEIGHT[0] * ((b >> 3) & 1) + RGWEIGHT[1] * ((b >> 4) & 1) + RGWEIGHT[2] * ((b >> 5) & 1) + 0.5);
        int bl = (int)(BWEIGHT[0] * ((b >> 6) & 1) + BWEIGHT[1] * ((b >> 7) & 1) + 0.5);
        rgb565_palette[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (bl >> 3));
    }
}

static inline uint16_t pen_color(uint8_t attr, uint8_t pixel2) {
    uint8_t pen = pacman_lookup_prom[(attr & 0x1Fu) * 4u + pixel2] & 0x0Fu;
    return rgb565_palette[pen];
}

// Renders one native row (0..PACMAN_GAME_HEIGHT-1) of tilemap + sprites
// into `out` (PACMAN_GAME_WIDTH RGB565 pixels). `system->flip_screen`
// flips both axes together, matching real cocktail-mode hardware.
static void render_native_row(const pacman_system *sys, uint32_t native_y, uint16_t *out) {
    bool flip = sys->flip_screen;
    uint32_t eff_y = flip ? (uint32_t)(PACMAN_GAME_HEIGHT - 1) - native_y : native_y;
    uint32_t row = eff_y >> 3, py = eff_y & 7u;

    // Background tilemap.
    for (uint32_t col = 0; col < 36; col++) {
        uint16_t idx = scan_rows(col, row);
        uint8_t tile = sys->video_ram[idx];
        uint8_t attr = sys->color_ram[idx];
        for (uint32_t px = 0; px < 8; px++) {
            uint32_t x = col * 8 + px;
            uint32_t out_x = flip ? (uint32_t)(PACMAN_GAME_WIDTH - 1) - x : x;
            out[out_x] = pen_color(attr, tile_pixels[tile][px][py]);
        }
    }

    // Sprites, descending index order (sprite 0 topmost) -- see this
    // file's header comment for the draw_sprites() ordering this mirrors.
    for (int s = 7; s >= 0; s--) {
        int offs = s * 2;
        uint8_t num = sys->sprite_num[offs];
        uint8_t code = num >> 2;
        bool fx = (num & 0x01) != 0;
        bool fy = (num & 0x02) != 0;
        uint8_t attr = sys->sprite_num[offs + 1];

        int sx = 272 - sys->sprite_pos[offs + 1];
        int sy = sys->sprite_pos[offs] - 31;
        if (s < 3) sy += 1; // xoffsethack -- see header comment

        int local_y = (int)eff_y - sy;
        // Also check the tunnel-wraparound copy at sx-256 (drawn
        // unconditionally by real hardware every frame, not just in a
        // tunnel -- it only ever lands on-screen when sx is near the
        // playfield edge).
        for (int copy = 0; copy < 2; copy++) {
            int this_sx = (copy == 0) ? sx : sx - 256;
            if (local_y < 0 || local_y >= 16) continue;
            int py2 = fy ? 15 - local_y : local_y;
            for (int col = 0; col < 16; col++) {
                int x = this_sx + col;
                // Real hardware clips sprites to tile columns 2..33
                // (x 16..271) -- see draw_sprites()'s `spriteclip`.
                if (x < 16 || x > 271) continue;
                int px2 = fx ? 15 - col : col;
                uint8_t pixel2 = sprite_pixels[code][px2][py2];
                if (pixel2 == 0) continue; // transparent
                uint32_t out_x = flip ? (uint32_t)(PACMAN_GAME_WIDTH - 1) - (uint32_t)x : (uint32_t)x;
                out[out_x] = pen_color(attr, pixel2);
            }
        }
    }
}

// Renders one native COLUMN (0..PACMAN_GAME_WIDTH-1) of tilemap + sprites
// into `out` (PACMAN_GAME_HEIGHT RGB565 pixels), producing exactly what
// taking that column out of every render_native_row() would.
//
// WHY THIS EXISTS. In yoko (landscape/180) one physical scanline IS a
// native column, so a row-only renderer cannot emit a single scanline until
// every row exists -- which is what forced the 129KB frame_cache and the
// whole-frame burst that starved the DVI queue every frame (DEVNOTES
// #18/#75). With a column primitive those orientations interleave exactly
// like tate, the cache is gone, and so is the red.
//
// It is a transposition of render_native_row(), not a reimplementation:
//   - `flip` maps display coords to internal ones on BOTH axes there, so
//     here the incoming display column is un-flipped once into `x` and the
//     outgoing display row is flipped per pixel.
//   - the tilemap walks 28 tile ROWS of one column instead of 36 tile
//     columns of one row (224 pixels rather than 288 -- slightly cheaper).
//   - sprites keep the same descending 7..0 order so sprite 0 stays
//     topmost, and the same two tunnel-wraparound copies; only the overlap
//     test changes axis, from "does this sprite cover eff_y" to "does it
//     cover x".
//
// Pac-Man draws all 8 sprites on every line with no per-line selection
// limit, which is what makes this a straight transposition. A machine whose
// hardware arbitrates sprites per scanline (Donkey Kong's 16-per-line
// buffer) cannot be transposed this way without precomputing that
// arbitration -- see DISPLAY_GEOMETRY.md section 7.
static void render_native_column(const pacman_system *sys, uint32_t native_x,
                                 uint16_t *out) {
    const bool flip = sys->flip_screen;
    const uint32_t x = flip ? (uint32_t)(PACMAN_GAME_WIDTH - 1) - native_x : native_x;
    const uint32_t tcol = x >> 3, px = x & 7u;

    // Background tilemap: one tile column, all 28 tile rows.
    for (uint32_t trow = 0; trow < 28; trow++) {
        const uint16_t idx  = scan_rows(tcol, trow);
        const uint8_t  tile = sys->video_ram[idx];
        const uint8_t  attr = sys->color_ram[idx];
        for (uint32_t py = 0; py < 8; py++) {
            const uint32_t eff_y = trow * 8u + py;
            const uint32_t out_y = flip ? (uint32_t)(PACMAN_GAME_HEIGHT - 1) - eff_y
                                        : eff_y;
            out[out_y] = pen_color(attr, tile_pixels[tile][px][py]);
        }
    }

    // Sprites, descending index order (sprite 0 topmost) -- same ordering
    // render_native_row() mirrors from draw_sprites().
    for (int sp = 7; sp >= 0; sp--) {
        const int offs = sp * 2;
        const uint8_t num  = sys->sprite_num[offs];
        const uint8_t code = num >> 2;
        const bool fx = (num & 0x01) != 0;
        const bool fy = (num & 0x02) != 0;
        const uint8_t attr = sys->sprite_num[offs + 1];

        const int sx = 272 - sys->sprite_pos[offs + 1];
        int sy = sys->sprite_pos[offs] - 31;
        if (sp < 3) sy += 1; // xoffsethack -- see header comment

        // Real hardware clips sprites to tile columns 2..33 (x 16..271) --
        // draw_sprites()'s `spriteclip`. In the row renderer this is a
        // per-pixel test; here the whole column is in or out at once.
        if ((int)x < 16 || (int)x > 271) continue;

        for (int copy = 0; copy < 2; copy++) {
            const int this_sx = (copy == 0) ? sx : sx - 256;
            const int col = (int)x - this_sx;   // sprite-local column
            if (col < 0 || col >= 16) continue;
            const int px2 = fx ? 15 - col : col;
            for (int local_y = 0; local_y < 16; local_y++) {
                const int eff_y = sy + local_y;
                if (eff_y < 0 || eff_y >= PACMAN_GAME_HEIGHT) continue;
                const int py2 = fy ? 15 - local_y : local_y;
                const uint8_t pixel2 = sprite_pixels[code][px2][py2];
                if (pixel2 == 0) continue; // transparent
                const uint32_t out_y = flip
                    ? (uint32_t)(PACMAN_GAME_HEIGHT - 1) - (uint32_t)eff_y
                    : (uint32_t)eff_y;
                out[out_y] = pen_color(attr, pixel2);
            }
        }
    }
}

// Renders the group of native columns that canvas row `dvi_y` collapses,
// merged into `out`.
//
// In yoko the raster's 288-column long axis has to fit 240 canvas rows, so
// 48 columns share a row with a neighbour. Sampling just one of them (plain
// nearest-neighbour) does not thin Pac-Man's 1-pixel maze walls, it deletes
// them -- 21 of the 288 columns contain picture and would never be drawn at
// all. Rendering the whole group and letting any non-background pixel win
// keeps them, at the cost of thickening some features by a pixel.
//
// Measured on this game: one column per canvas row keeps 58.2% of the lit
// pixels; merging this axis and the emit axis together keeps 82.3%. Costs
// ~48 extra render_native_column() calls a frame, about +20% of the column
// render. See DEVNOTES #80.
//
// `first` is the group's first native column in DISPLAY order and `step` is
// +1 or -1, because case 0 walks the axis reversed and case 2 does not.
static void render_native_column_group(const pacman_system *sys, uint32_t first,
                                       int step, uint32_t count, uint16_t *out) {
    render_native_column(sys, first, out);
    if (count < 2u) return;

    static uint16_t extra[PACMAN_GAME_HEIGHT];
    for (uint32_t k = 1; k < count; k++) {
        render_native_column(sys, (uint32_t)((int)first + step * (int)k), extra);
        for (uint32_t i = 0; i < (uint32_t)PACMAN_GAME_HEIGHT; i++)
            if (extra[i]) out[i] = extra[i];
    }
}

// THE 129KB frame_cache THAT USED TO LIVE HERE IS GONE. It existed because
// this file could only render rows: a yoko scanline reads one pixel from
// every native row, so all 224 had to exist before the first scanline could
// be emitted, and building them was a whole-frame burst with no
// acquire/submit calls in it at all. Core 1 then had only its 8-buffer
// queue (~555us) to coast on and painted PicoDVI's "no valid scanline
// ready" red for the rest -- DEVNOTES #18, and #75 for the measurement that
// showed Donkey Kong doing it ~150 times a frame. render_native_column()
// above removes the reason for the cache, so yoko now renders on demand and
// interleaves exactly like tate. Note the cache was a static, so it cost
// its full 129KB in EVERY orientation, including the tate default where it
// was never read.
//
// pacman_video_render_scanline() is exposed here (not static) so
// pacman_machine.cpp's pacman_run_frame() can call it directly, once per
// scanline, INTERLEAVED with the Z80 cycles that update the VRAM/sprite
// state it reads -- see that function's own comment (DEVNOTES.md problem
// #19) for why: running an entire frame's ~50,688 Z80 cycles in one
// uninterrupted burst before EVER calling hal_video_acquire_scanline() is
// the same starvation mechanism, just moved into the CPU loop instead of
// the renderer.

void pacman_video_render_scanline(const pacman_system *sys, uint32_t dvi_y, uint16_t *buf) {
    memset(buf, 0, HAL_VIDEO_WIDTH * sizeof(uint16_t));
    bool mir = sys->mirror_x;
    static uint16_t row[PACMAN_GAME_WIDTH]; // scratch for tate's on-demand row
    static uint16_t col[PACMAN_GAME_HEIGHT]; // scratch for yoko's on-demand column

    switch (sys->rotation) {

    case 0: {
        // Landscape. This scanline IS a native column: av_yoko.row maps the
        // canvas row onto the raster's LONG axis to pick which one, and
        // av_yoko.col maps canvas columns onto the SHORT axis -- which in
        // yoko must NARROW to 180 canvas columns, not sit at 1:1.
        //
        // The `(W-1) - dy` reversal is NOT cosmetic: a 90-degree rotation
        // must reverse exactly one axis relative to tate (case 1, which
        // does not reverse its equivalent), or the result is a mirror image
        // rather than a rotation (DEVNOTES #21).
        render_native_column_group(
            sys, (uint32_t)(PACMAN_GAME_WIDTH - 1) - av_yoko.row[dvi_y], -1,
            av_yoko.rowrep[dvi_y], col);
        // The emit MERGES rather than samples: yoko narrows the short axis
        // (224 -> 180 canvas columns) and dropping the difference deletes
        // whole maze walls. See av_emit_row_merge().
        if (mir) av_emit_row_merge_rev(buf, col, &av_yoko);
        else     av_emit_row_merge(buf, col, &av_yoko);
        break;
    }

    case 1: {
        // 90 deg CCW (tate). Rendered on demand, one raster row per call --
        // rendered on demand from live VRAM, one raster row per call.
        if (dvi_y < av_tate.y0 || dvi_y >= av_tate.y1) return;
        uint32_t dx = av_tate.row[dvi_y];
        if (mir) dx = (uint32_t)(PACMAN_GAME_HEIGHT - 1) - dx;
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
        // The 1:1 branch is a MEASURED requirement, not tidiness: going
        // through av_tate.col[] unconditionally cost this family +1.6ms a
        // frame and put Donkey Kong's work_max past the budget. See
        // av_map_t::col_1to1.
        if (av_tate.col_1to1) {
            uint16_t *out = buf + av_tate.x0;
            for (uint32_t c = 0; c < (uint32_t)PACMAN_GAME_WIDTH; c++) out[c] = row[c];
        } else {
            av_emit_row(buf, row, &av_tate);
        }
        break;
    }

    case 2: {
        // 180 deg: case 0 with BOTH axes reversed relative to it (a 180 is
        // two 90s), so the column index is deliberately the *un*-reversed
        // raw value -- case 0 already reversed it once, and reversing that
        // again would cancel out -- while the emit direction flips.
        render_native_column_group(sys, av_yoko.row[dvi_y], +1,
                                   av_yoko.rowrep[dvi_y], col);
        if (mir) av_emit_row_merge(buf, col, &av_yoko);
        else     av_emit_row_merge_rev(buf, col, &av_yoko);
        break;
    }

    case 3: {
        // 90 deg CW (tate) -- case 1 with both the scanline order and the
        // within-scanline order reversed.
        if (dvi_y < av_tate.y0 || dvi_y >= av_tate.y1) return;
        const uint32_t d  = av_tate.row[dvi_y];
        const uint32_t dx = mir ? d : (uint32_t)(PACMAN_GAME_HEIGHT - 1) - d;
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
            for (uint32_t c = 0; c < (uint32_t)PACMAN_GAME_WIDTH; c++)
                out[c] = row[(uint32_t)(PACMAN_GAME_WIDTH - 1u) - c];
        } else {
            av_emit_row_rev(buf, row, &av_tate);
        }
        break;
    }

    default: break;
    }
}

void pacman_draw_error_frame(uint16_t color) {
    for (uint32_t i = 0; i < HAL_VIDEO_HEIGHT; i++) {
        uint16_t *buf = hal_video_acquire_scanline();
        for (uint32_t x = 0; x < HAL_VIDEO_WIDTH; x++) buf[x] = color;
        hal_video_submit_scanline(buf);
    }
}
