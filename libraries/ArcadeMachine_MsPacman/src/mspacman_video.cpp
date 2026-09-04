// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Ms. Pac-Man tile+sprite renderer -- a verbatim copy of
// ArcadeMachine_Pacman's pacman_video.cpp (same board, same video
// hardware, same PROMs). Kept as its own file rather than shared because
// SAMP's machine axis is one library per machine; see this project's
// README for that trade-off.
// ORIGINAL COMMENT FOLLOWS, unchanged:
//
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
//   and verified by hand against the real mspacman_assets/rom/82s123.7f
//   bytes -- e.g. byte 0x07 (pen 1, Blinky's red) decodes to (255,0,0),
//   byte 0xC9 (pen 11, the maze's iconic blue) decodes to (33,33,255).
// - Color lookup PROM (82s126.4a): pen = lookup[attr*4 + pixel2bit] &
//   0x0F, where attr is colorram/spriteram's low 5 bits (colortablebank/
//   palettebank are always 0 on this hardware -- those exist for sibling
//   games sharing this video chip, e.g. Pengo).
#include <string.h>
#include "mspacman_video.h"
#include "arcade_hal_video.h"

uint8_t mspacman_gfx_rom[MSPACMAN_GFX_ROM_SIZE];
uint8_t mspacman_palette_prom[MSPACMAN_PALETTE_PROM_SIZE];
uint8_t mspacman_lookup_prom[MSPACMAN_LOOKUP_PROM_SIZE];

// Decoded caches, built once by mspacman_video_build_caches().
static uint8_t tile_pixels[256][8][8];   // [tile][x][y] -> 2bpp pixel (0-3)
static uint8_t sprite_pixels[64][16][16]; // [sprite][x][y] -> 2bpp pixel (0-3)
static uint16_t rgb565_palette[32];

// Border/scale constants for a 640x480 4:3 monitor, following the exact
// pattern invaders_video.cpp established (see that file's header comment
// for the full derivation and invaders_pico's DEVNOTES.md for why the
// real visible window on this board is DVI x 0..319, not the full 640).
// MSPACMAN_GAME_HEIGHT (224) matches INVADERS_GAME_HEIGHT exactly (both are
// classic 224-line arcade rasters), so the vertical constant transfers
// unchanged; only TATE_BX differs, since Pac-Man's raw width (288) differs
// from Invaders' (256). As with the original tuning, fine pixel alignment
// should be confirmed on real hardware (see CLAUDE.md -- flashing and
// observing the physical display is the only real verification here).
#define TATE_BY   8u    // (240 - 224) / 2
#define TATE_BX  16u    // (320 - 288)   / 2
#define LAND_BX  48u    // (320 - 224)   / 2 -- centred in visible DVI x 0..319

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

void mspacman_video_build_caches(void) {
    // --- Tiles: gfx1 offset 0x0000, 16 bytes/tile, 256 tiles. ---
    for (int t = 0; t < 256; t++) {
        const uint8_t *base = &mspacman_gfx_rom[t * 16];
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
        const uint8_t *base = &mspacman_gfx_rom[0x1000 + s * 64];
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
        uint8_t b = mspacman_palette_prom[i];
        int r = (int)(RGWEIGHT[0] * ((b >> 0) & 1) + RGWEIGHT[1] * ((b >> 1) & 1) + RGWEIGHT[2] * ((b >> 2) & 1) + 0.5);
        int g = (int)(RGWEIGHT[0] * ((b >> 3) & 1) + RGWEIGHT[1] * ((b >> 4) & 1) + RGWEIGHT[2] * ((b >> 5) & 1) + 0.5);
        int bl = (int)(BWEIGHT[0] * ((b >> 6) & 1) + BWEIGHT[1] * ((b >> 7) & 1) + 0.5);
        rgb565_palette[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (bl >> 3));
    }
}

static inline uint16_t pen_color(uint8_t attr, uint8_t pixel2) {
    uint8_t pen = mspacman_lookup_prom[(attr & 0x1Fu) * 4u + pixel2] & 0x0Fu;
    return rgb565_palette[pen];
}

// Renders one native row (0..MSPACMAN_GAME_HEIGHT-1) of tilemap + sprites
// into `out` (MSPACMAN_GAME_WIDTH RGB565 pixels). `system->flip_screen`
// flips both axes together, matching real cocktail-mode hardware.
static void render_native_row(const mspacman_system *sys, uint32_t native_y, uint16_t *out) {
    bool flip = sys->flip_screen;
    uint32_t eff_y = flip ? (uint32_t)(MSPACMAN_GAME_HEIGHT - 1) - native_y : native_y;
    uint32_t row = eff_y >> 3, py = eff_y & 7u;

    // Background tilemap.
    for (uint32_t col = 0; col < 36; col++) {
        uint16_t idx = scan_rows(col, row);
        uint8_t tile = sys->video_ram[idx];
        uint8_t attr = sys->color_ram[idx];
        for (uint32_t px = 0; px < 8; px++) {
            uint32_t x = col * 8 + px;
            uint32_t out_x = flip ? (uint32_t)(MSPACMAN_GAME_WIDTH - 1) - x : x;
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
                uint32_t out_x = flip ? (uint32_t)(MSPACMAN_GAME_WIDTH - 1) - (uint32_t)x : (uint32_t)x;
                out[out_x] = pen_color(attr, pixel2);
            }
        }
    }
}

// Full native frame cache (288x224 RGB565 = ~129KB -- affordable on the
// RP2350B's 520KB SRAM), used ONLY by landscape/180-degree rotation
// (cases 0/2 below), which genuinely need it: a single physical scanline
// in those orientations reads one pixel from EVERY native row (a column
// slice), so all 224 rows must exist before that scanline can be emitted
// at all -- there is no way to compute those modes one row at a time.
//
// Tate (cases 1/3, the default) does NOT use this cache -- it renders its
// one needed native row on demand, directly in
// mspacman_video_render_scanline() below, same as invaders_video.cpp/
// lrescue_video.cpp's per-call renderers. GETTING THIS WRONG WAS A REAL
// BUG, found on real hardware: an earlier version of this file
// unconditionally rendered the full frame_cache *before* calling
// hal_video_acquire_scanline() even once. That starves Core 1's DVI
// scanline queue for the entire ~224-row render burst (no acquire/submit
// calls happen during it at all), which PicoDVI shows as its own explicit
// "no valid scanline ready" solid-red fallback (see arcade_arduino/
// DEVNOTES.md problem #18) -- reported as most of the screen going red
// with only a sliver of real picture getting through.
// hal_video_acquire_scanline()/hal_video_submit_scanline() exist
// specifically to pace Core 0 against Core 1's fixed real DVI rate one
// scanline at a time; any renderer on this framework must call them from
// inside its per-scanline loop, never do a large uninterrupted compute
// burst before the first call in a frame.
//
// mspacman_video_render_scanline() is exposed here (not static) so
// mspacman_machine.cpp's mspacman_run_frame() can call it directly, once per
// scanline, INTERLEAVED with the Z80 cycles that update the VRAM/sprite
// state it reads -- see that function's own comment (DEVNOTES.md problem
// #19) for why: even with tate's on-demand rendering fixed, running an
// entire frame's ~50,688 Z80 cycles in one uninterrupted burst before
// EVER calling hal_video_acquire_scanline() is the exact same starvation
// mechanism, just moved into the CPU loop instead of the renderer.
static uint16_t frame_cache[MSPACMAN_GAME_HEIGHT][MSPACMAN_GAME_WIDTH];

void mspacman_video_render_scanline(const mspacman_system *sys, uint32_t dvi_y, uint16_t *buf) {
    memset(buf, 0, HAL_VIDEO_WIDTH * sizeof(uint16_t));
    bool mir = sys->mirror_x;
    static uint16_t row[MSPACMAN_GAME_WIDTH]; // scratch for tate's on-demand row

    switch (sys->rotation) {

    case 0: {
        // Landscape: same shape as invaders_video.cpp's case 0 -- the raw
        // hardware's horizontal axis (288, "col") stretches to fill all
        // 480 DVI scanlines; its vertical axis (224, "dx"/native row)
        // fills the 320-wide visible window 1:1. The `(W-1) - dy` here is
        // NOT cosmetic -- a 90-degree rotation must reverse the direction
        // of exactly one axis relative to tate mode (case 1, which does
        // NOT reverse its equivalent axis), or the result is a mirror
        // image instead of a rotation. Ported verbatim from
        // invaders_video.cpp's case 0 (real-hardware-verified there) --
        // an earlier version of this file dropped this reversal, which is
        // exactly what made switching from tate to landscape look
        // mirrored on real hardware.
        uint32_t dy = ((uint32_t)dvi_y * (uint32_t)MSPACMAN_GAME_WIDTH) / HAL_VIDEO_HEIGHT;
        uint32_t col = (uint32_t)(MSPACMAN_GAME_WIDTH - 1) - dy;
        for (uint32_t i = 0; i < (uint32_t)MSPACMAN_GAME_HEIGHT; i++) {
            uint32_t dx = mir ? (uint32_t)(MSPACMAN_GAME_HEIGHT - 1) - i : i;
            buf[LAND_BX + i] = frame_cache[dx][col];
        }
        break;
    }

    case 1: {
        // 90 deg CCW (tate, default): DVI y -> native row (x2 scale).
        // Computed here, on demand, one row per call -- see this cache's
        // header comment for why tate must NOT read frame_cache.
        if (dvi_y < TATE_BY || dvi_y >= TATE_BY + (uint32_t)MSPACMAN_GAME_HEIGHT) return;
        uint32_t dx = dvi_y - TATE_BY;
        if (mir) dx = (uint32_t)(MSPACMAN_GAME_HEIGHT - 1) - dx;
        render_native_row(sys, dx, row);
        for (uint32_t col = 0; col < (uint32_t)MSPACMAN_GAME_WIDTH; col++)
            buf[TATE_BX + col] = row[col];
        break;
    }

    case 2: {
        // 180 deg (landscape upside-down): case 0 with BOTH axes reversed
        // relative to it (a 180 is two 90s) -- so `col` here is
        // deliberately the *un*-reversed raw value (case 0 already
        // reversed it once; reversing case 0's own reversal again would
        // just cancel out, matching invaders_video.cpp's case 2 exactly),
        // while `dx`'s ternary below is the mirror image of case 0's.
        uint32_t col = ((uint32_t)dvi_y * (uint32_t)MSPACMAN_GAME_WIDTH) / HAL_VIDEO_HEIGHT;
        for (uint32_t i = 0; i < (uint32_t)MSPACMAN_GAME_HEIGHT; i++) {
            uint32_t dx = mir ? i : (uint32_t)(MSPACMAN_GAME_HEIGHT - 1) - i;
            buf[LAND_BX + i] = frame_cache[dx][col];
        }
        break;
    }

    case 3: {
        // 90 deg CW (tate) -- on demand, same as case 1.
        if (dvi_y < TATE_BY || dvi_y >= TATE_BY + (uint32_t)MSPACMAN_GAME_HEIGHT) return;
        uint32_t dx = mir ? (dvi_y - TATE_BY)
                          : (uint32_t)(MSPACMAN_GAME_HEIGHT - 1) - (dvi_y - TATE_BY);
        render_native_row(sys, dx, row);
        for (uint32_t col = 0; col < (uint32_t)MSPACMAN_GAME_WIDTH; col++) {
            uint32_t rev = (uint32_t)(MSPACMAN_GAME_WIDTH - 1u) - col;
            buf[TATE_BX + col] = row[rev];
        }
        break;
    }

    default: break;
    }
}

void mspacman_draw_frame(mspacman_system *system) {
    // Only landscape/180 (cases 0/2) need the full frame_cache -- see its
    // header comment. This is still a real, not-yet-optimized stall risk
    // for those two orientations (same queue-starvation mechanism the
    // comment above describes) -- tate (the default, cases 1/3) is the
    // one confirmed fixed so far.
    if (system->rotation == 0 || system->rotation == 2) {
        for (uint32_t y = 0; y < (uint32_t)MSPACMAN_GAME_HEIGHT; y++)
            render_native_row(system, y, frame_cache[y]);
    }

    for (uint32_t i = 0; i < HAL_VIDEO_HEIGHT; i++) {
        uint16_t *buf = hal_video_acquire_scanline();
        mspacman_video_render_scanline(system, i, buf);
        hal_video_submit_scanline(buf);
    }
}

void mspacman_draw_error_frame(uint16_t color) {
    for (uint32_t i = 0; i < HAL_VIDEO_HEIGHT; i++) {
        uint16_t *buf = hal_video_acquire_scanline();
        for (uint32_t x = 0; x < HAL_VIDEO_WIDTH; x++) buf[x] = color;
        hal_video_submit_scanline(buf);
    }
}
