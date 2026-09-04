// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Burger Time video. Every formula here is a transcription of a specific
// function in MAME's src/mame/dataeast/btime.cpp; the names in the comments
// are that file's own.
//
// WHAT THE HARDWARE DRAWS, in the order screen_update_btime() draws it:
//
//     if (bnj_scroll[0] & 0x10) { draw_background(); draw_chars(TRANSPARENT); }
//     else                      { draw_chars(OPAQUE); }
//     draw_sprites();                          // always, pen 0 transparent
//
//   - CHARS: a 32x32 grid of 8x8 3bpp characters, 1024 of them, on palette
//     pens 0-7. Code = videoram[offs] + 256 * (colorram[offs] & 3) -- so
//     colour RAM's low two bits are CODE bits 8-9, not a colour selector.
//     The whole charset shares one palette.
//   - SPRITES: 8 of them, 16x16 3bpp, ALSO on pens 0-7 and ALSO decoded out
//     of the same gfx1 region as the chars (two different layouts over the
//     same bytes). Their attributes live INSIDE video RAM, interleaved at
//     stride 0x20.
//   - BACKGROUND: 16x16 3bpp tiles from gfx2 on pens 8-15, arranged by a
//     256-entry page of the bg_map ROM, enabled by a port bit.
//
// DECODE CACHES FOR CHARS AND BACKGROUND TILES, SPRITES ON DEMAND. The
// first version of this file had no caches at all, on the reasoning that
// Burger Time's graphics are straight bitplanes (unlike Pac-Man's and
// Donkey Kong's awkward interleaved nibbles), so extracting one row of one
// character is just three byte reads plus a bit-spread -- cheap enough to
// do every frame, and worth 80KB of RAM.
//
// THAT REASONING WAS WRONG, and measurement is what showed it. On the first
// hardware flash the screen went red (a starved DVI queue) and the music
// ran slow; the host harness's cost breakdown put the blame precisely:
//
//     CPUs    91us   33.6%
//     render 168us   62.0%      <-- this file
//     audio   12us    4.4%
//
// The renderer alone cost twice Donkey Kong's ENTIRE frame. Three byte
// reads plus a spread is indeed cheap per pixel, but this machine draws a
// lot of pixels per row -- 32 char cells plus, when the background layer is
// on, another two page-copies of sixteen 16-pixel tiles -- and cheap times
// a quarter of a million is not cheap. See DEVNOTES.md #59.
//
// So the caches the old comment called "the obvious lever" are now here:
//
//     char_px[1024][8][8]   64 KB   a char row is an 8-byte copy
//     bg_px[64][16][16]     16 KB   a tile row is a 16-byte copy
//     spread[256][8]         2 KB   still used to BUILD those, and for sprites
//
// Sprites stay on demand deliberately: only 8 exist, so the whole frame's
// sprite work is 8 x 16 x 16 = 2048 pixels, and caching all 256 possible
// ones would cost another 64KB to save nothing measurable.
//
// The renderer is row-oriented for the same reason the other tate renderers
// are: one submitted DVI scanline is one raster row, rendered on demand from
// live VRAM, interleaved with CPU execution (DEVNOTES.md #19/#20/#34/#36).
//
// COORDINATES ARE RAW 0..255 ON BOTH AXES, not 0..239. MAME's screen bitmap
// for this driver is the full htotal x vtotal and the visible window is the
// cliprect x/y 8..247, so every coordinate formula below is in raw space and
// the 8-pixel crop is applied once, on output. Mixing the two shifts the
// entire picture by 8 pixels diagonally, which reads as a tile-addressing
// bug rather than a cropping mistake.
#include <string.h>
#include "btime_video.h"
#include "arcade_hal_video.h"
#include "arcade_video_geom.h"

// SRAM placement for the per-scanline render path. Measured at 36% of the
// frame in the host harness after the decode caches landed (and 62% before
// them), so it is the second-hottest thing here after the interpreters --
// and on device it pays twice, because a row renderer walking cache tables
// out of flash stalls on XIP misses exactly when the DVI queue can least
// afford it. Same raw section attribute and reasoning as ArcadeCPU_M6502's
// m6502_step(), ArcadeCPU_Z80's z80_step() and galaga_ports.cpp; guarded so
// the host harness compiles unchanged.
#if defined(ARDUINO_ARCH_RP2040) || defined(PICO_ON_DEVICE)
#define BTIME_VRAMFUNC __attribute__((section(".time_critical.btimevid")))
#else
#define BTIME_VRAMFUNC
#endif

uint8_t btime_gfx1[BTIME_GFX1_SIZE];
uint8_t btime_gfx2[BTIME_GFX2_SIZE];
uint8_t btime_bg_map[BTIME_BG_MAP_SIZE];

// Plane bases. From gfx_8x8x3_planar (src/emu/video/generic.cpp) and
// tile16layout (btime.cpp:2077), both of which list their planes as
// { RGN_FRAC(2,3), RGN_FRAC(1,3), RGN_FRAC(0,3) }, and from
// gfx_element::decode() (src/emu/drawgfx.cpp:298), which walks the plane
// list starting at `planebit = 1 << (planes - 1)`. So planeoffset[0] is the
// MOST significant pen bit: the 2/3 chunk is pen bit 2, and the 0/3 chunk
// is pen bit 0.
#define GFX1_PLANE2 0x4000 // MSB
#define GFX1_PLANE1 0x2000
#define GFX1_PLANE0 0x0000
#define GFX2_PLANE2 0x1000 // MSB
#define GFX2_PLANE1 0x0800
#define GFX2_PLANE0 0x0000

// Background tiles are GFXDECODE_ENTRY("gfx2", 0, tile16layout, 8, 1) --
// palette base 8. Chars and sprites are base 0.
#define BG_PEN_BASE 8

// --- geometry ------------------------------------------------------------
//
// The board's visible framebuffer is 320x240 logical square pixels (640x480
// timing, only DVI x 0..319 visible, dvi_vertical_repeat 2 so one submitted
// scanline is two physical rows) -- see invaders_video.cpp's header, which
// calibrated these against real hardware, and hal_video_fruitjam.cpp.
//
// Tate maps the raster's vertical axis onto the 240 submitted scanlines and
// its horizontal axis along the scanline buffer. Burger Time's raster is
// SQUARE, so unlike its siblings both constants come out the same:

// ...and unlike its siblings, 1:1 is visibly wrong. In tate the monitor is
// physically rotated, so the raster's horizontal axis lands on the screen's
// long (4-unit) side and its vertical axis on the short (3-unit) side. A
// real ROT270 cabinet fills that portrait screen: 4 units by 3. Every game
// in this project uses x1 along the long axis and so comes out short:
//
//     Pac-Man          288/320 -> 3.6 of 4 units    3.6% compressed
//     Space Invaders   256/320 -> 3.2 of 4 units     14% compressed
//     Burger Time      240/320 -> 3.0 of 4 units     25% compressed
//
// i.e. the picture would be 3/4 of its correct height -- by a wide margin
// the worst in the project, because this is the only square raster here.
// The fix is to spread the raster's horizontal axis over all 320 columns
// (nx = col * 3 / 4), which also makes this the only game here that fills
// the screen edge to edge.
//
// DEFAULT IS 1:1 FOR NOW, ON PURPOSE. The pillarbox makes the geometry
// unambiguous to eyeball at first light and keeps the harness's PPM output
// integer-scaled; the stretch is one call away and the two should be
// compared on the physical display before either becomes the shipped
// default. See BTIME_PORT_PLAN.md section 5.7.
// Kept as this game's existing API, now delegating to the shared
// aspect correction so there is one switch for the whole project.
void btime_video_set_aspect_stretch(bool enable) { av_geom_set_stretch(enable); }
bool btime_video_get_aspect_stretch(void) { return av_geom_get_stretch(); }

// --- palette -------------------------------------------------------------
//
// This board has no colour PROM. MAME's own comment, above
// btime_palette():
//
//     Burger Time doesn't have a color PROM. It uses RAM to dynamically
//     create the palette. The palette RAM is connected to the RGB output
//     this way:
//
//     bit 7 -- 15 kohm resistor  -- BLUE (inverted)
//           -- 33 kohm resistor  -- BLUE (inverted)
//           -- 15 kohm resistor  -- GREEN (inverted)
//           -- 33 kohm resistor  -- GREEN (inverted)
//           -- 47 kohm resistor  -- GREEN (inverted)
//           -- 15 kohm resistor  -- RED (inverted)
//           -- 33 kohm resistor  -- RED (inverted)
//     bit 0 -- 47 kohm resistor  -- RED (inverted)
//
// and the format declared in btime()'s machine_config is
// palette_device::BGR_233_inverted with 16 entries: invert the byte, then
// red = bits 0-2, green = bits 3-5, blue = bits 6-7.
//
// The enum's own spelling in emupal.h is the bit layout: `enum
// bgr_233_inv_t { BGR_233_inverted, BBGGGRRR_inverted }` -- from the top
// bit down, two bits of blue, three of green, three of red, all inverted.
//
// The 3->8 and 2->8 bit expansion is MAME's pal3bit/pal2bit, which are BIT
// REPLICATION, not an even i*255/7 scaling (src/lib/util/palette.h,
// palexpand<>):
//     3 bits: (b << 5) | (b << 2) | (b >> 1)
//     2 bits: (b << 6) | (b << 4) | (b << 2) | b
// Replication and even scaling agree on six of the eight 3-bit values and
// differ by 1/255 on the other two, which is invisible here -- but this
// project's rule is to match the cited source rather than something that
// rounds to nearly the same thing, so replication it is.
//
// Note also that this does NOT weight by the resistor values the way
// ArcadeMachine_DKong's resnet port does. That is MAME's choice for this
// driver, and matching MAME is the point -- MAME's output is what these
// colours can actually be checked against.
static uint16_t pal565[16];

static inline uint16_t rgb565_from_palette_byte(uint8_t value) {
    const uint8_t v = (uint8_t)~value; // "(inverted)" in the comment above
    const uint8_t r3 = (uint8_t)(v & 0x07);
    const uint8_t g3 = (uint8_t)((v >> 3) & 0x07);
    const uint8_t b2 = (uint8_t)((v >> 6) & 0x03);
    const uint8_t r8 = (uint8_t)((r3 << 5) | (r3 << 2) | (r3 >> 1)); // pal3bit
    const uint8_t g8 = (uint8_t)((g3 << 5) | (g3 << 2) | (g3 >> 1)); // pal3bit
    const uint8_t b8 = (uint8_t)((b2 << 6) | (b2 << 4) | (b2 << 2) | b2); // pal2bit
    return (uint16_t)(((r8 & 0xF8) << 8) | ((g8 & 0xFC) << 3) | (b8 >> 3));
}

void btime_video_palette_write(uint8_t index, uint8_t value) {
    if (index < 16) pal565[index] = rgb565_from_palette_byte(value);
}

// --- bit spread ----------------------------------------------------------
//
// spread[b][j] is bit (7-j) of b, i.e. the byte expanded MSB-first into one
// byte per pixel. Both graphics layouts here are MSB-first along x:
// gfx_8x8x3_planar's xoffs is STEP8(0,1) and tile16layout's is
// { STEP8(16*8,1), STEP8(0,1) }, and MAME numbers bit offsets from the top
// bit of each byte, so pixel x=0 comes from bit 7.
static uint8_t spread[256][8];

// Decoded pixel caches, [code][row][col] so that one row of one character
// or tile is CONTIGUOUS -- that is the whole point, since the renderer walks
// rows. (Deliberately NOT ArcadeMachine_Pacman's [tile][x][y] order, which
// suits a different access pattern.)
//
// char_px holds 3bpp pens 0-7. bg_px holds 8-15: the background's +8
// palette base is BAKED IN here rather than added per pixel at draw time,
// which turns a tile row from sixteen add-and-store pairs into a memcpy.
// That is safe because the two caches are never mixed -- the char layer's
// transparency test looks at char pens, never at background ones.
static uint8_t char_px[1024][8][8];
static uint8_t bg_px[64][16][16];

// COLUMN-MAJOR TWINS of the two caches above: [code][col][row] rather than
// [code][row][col]. 64KB + 16KB, and they exist for one measured reason.
//
// This game's row renderer was optimised into memcpy()s -- a char row is an
// 8-byte copy, a background tile row a 16-byte copy -- and that is what took
// it from red to a flat 60fps (DEVNOTES #59). Transposing it for yoko turned
// every one of those copies into per-pixel indexed reads down a stride, and
// measured on hardware that cost the renderer 4950us -> 9762us a frame,
// against ~1.1ms of headroom. With these, a column read is the same 8- or
// 16-byte copy a row read is, and the two orientations cost the same.
//
// Worth stating plainly because the intuition is wrong: on this part SRAM
// is single-cycle and uniform, so the stride itself is not what hurt. What
// hurt was replacing a handful of word-wide copies with 256 separate
// byte loads, each with its own three-level index arithmetic.
static uint8_t char_px_T[1024][8][8];   // [code][col][row]
static uint8_t bg_px_T[64][16][16];     // [code][col][row]

void btime_video_build_caches(void) {
    for (int b = 0; b < 256; b++)
        for (int j = 0; j < 8; j++)
            spread[b][j] = (uint8_t)((b >> (7 - j)) & 1);

    // Characters: gfx_8x8x3_planar, 8 bytes per plane per char, 1024 chars.
    for (int code = 0; code < 1024; code++) {
        for (int row = 0; row < 8; row++) {
            const uint8_t *base = &btime_gfx1[code * 8 + row];
            const uint8_t *s0 = spread[base[GFX1_PLANE0]];
            const uint8_t *s1 = spread[base[GFX1_PLANE1]];
            const uint8_t *s2 = spread[base[GFX1_PLANE2]];
            for (int j = 0; j < 8; j++) {
                const uint8_t pen = (uint8_t)((s2[j] << 2) | (s1[j] << 1) | s0[j]);
                char_px[code][row][j]   = pen;
                char_px_T[code][j][row] = pen;
            }
        }
    }

    // Background tiles: tile16layout over gfx2, 32 bytes per plane per tile,
    // 64 tiles. Same left/right byte split as the sprites (see
    // draw_sprite_row_at()): x 0..7 from byte 16+row, x 8..15 from byte row.
    for (int code = 0; code < 64; code++) {
        for (int row = 0; row < 16; row++) {
            const uint32_t tile = (uint32_t)code * 32u + (uint32_t)row;
            const uint8_t *lo = &btime_gfx2[tile + 16u];
            const uint8_t *hi = &btime_gfx2[tile];
            const uint8_t *l0 = spread[lo[GFX2_PLANE0]];
            const uint8_t *l1 = spread[lo[GFX2_PLANE1]];
            const uint8_t *l2 = spread[lo[GFX2_PLANE2]];
            const uint8_t *h0 = spread[hi[GFX2_PLANE0]];
            const uint8_t *h1 = spread[hi[GFX2_PLANE1]];
            const uint8_t *h2 = spread[hi[GFX2_PLANE2]];
            for (int j = 0; j < 8; j++) {
                const uint8_t lp = (uint8_t)(BG_PEN_BASE + ((l2[j] << 2) | (l1[j] << 1) | l0[j]));
                const uint8_t hp = (uint8_t)(BG_PEN_BASE + ((h2[j] << 2) | (h1[j] << 1) | h0[j]));
                bg_px[code][row][j]         = lp;
                bg_px[code][row][8 + j]     = hp;
                bg_px_T[code][j][row]       = lp;
                bg_px_T[code][8 + j][row]   = hp;
            }
        }
    }

    // Palette RAM powers up as whatever the game writes; until it does,
    // every pen is the inverted-zero colour (white). Build the initial
    // table from the reset state so a frame rendered before the game
    // touches palette RAM shows something rather than uninitialised memory.
    for (int i = 0; i < 16; i++) pal565[i] = rgb565_from_palette_byte(0);
}

// --- one raster row ------------------------------------------------------
//
// pen_row holds RAW x 0..255 for the row being built. Both the char layer
// (32 cells x 8px) and the background layer cover it completely, so it does
// not need clearing between rows.
static uint8_t pen_row[BTIME_RASTER];

// draw_chars(), btime.cpp:655. MAME iterates video RAM and computes where
// each cell lands:
//     uint8_t x = 31 - (offs / 32);
//     uint8_t y = offs % 32;
//     code = m_videoram[offs] + 256 * (m_colorram[offs] & 3);
//     if (flip_screen()) { x = 31 - x; y = 31 - y; }
//     gfx(0)->transpen(..., code, 0, flip, flip, 8*x, 8*y, transparency ? 0 : -1);
//
// Inverted for a row-oriented renderer: the cell covering raster row
// `raw_y` at screen column `cx` is at video RAM offset
//     32 * (31 - cx) + (raw_y >> 3)          unflipped
//     32 * cx + (31 - (raw_y >> 3))          flipped
// and the character row drawn there is (raw_y & 7), or 7 - (raw_y & 7) when
// flipped (transpen is passed flipy = flip_screen()).
BTIME_VRAMFUNC static void draw_chars_row(const btime_system *s, uint32_t raw_y,
                           bool transparent) {
    const bool flip = s->flip_screen;
    const uint32_t cy  = raw_y >> 3;
    const uint32_t sub = raw_y & 7u;
    const uint32_t char_row = flip ? (7u - sub) : sub;

    for (uint32_t cx = 0; cx < 32; cx++) {
        const uint16_t offs = flip ? (uint16_t)(32u * cx + (31u - cy))
                                   : (uint16_t)(32u * (31u - cx) + cy);
        const uint16_t code = (uint16_t)(s->videoram[offs] |
                                        ((s->colorram[offs] & 3u) << 8));

        const uint8_t *src = char_px[code][char_row];
        uint8_t *out = &pen_row[cx * 8u];

        // The common case by far -- background off, so chars are opaque and
        // unflipped -- is a straight 8-byte copy.
        if (!transparent && !flip) {
            memcpy(out, src, 8);
        } else if (!flip) {
            for (uint32_t j = 0; j < 8; j++)
                if (src[j]) out[j] = src[j];
        } else {
            for (uint32_t j = 0; j < 8; j++) {
                const uint8_t pen = src[j];
                if (transparent && pen == 0) continue;
                out[7u - j] = pen;
            }
        }
    }
}

// draw_sprites(), btime.cpp:683, called by screen_update_btime() as
//     draw_sprites(bitmap, cliprect, 0, 1, 0, m_videoram, 0x20);
// i.e. colour 0, sprite_y_adjust 1, sprite_y_adjust_flip_screen 0, sprite
// RAM = video RAM, interleave 0x20.
//
// Sprite i's four attribute bytes are at videoram[i*0x80 + {0, 0x20, 0x40,
// 0x60}] -- the "first row of the swapped area", which through the normal
// decoder is the first COLUMN of video RAM (see this driver's header).
//
//     if (!(sprite_ram[offs + 0] & 0x01)) continue;   // enable
//     int x = 240 - sprite_ram[offs + 3 * interleave];
//     int y = 240 - sprite_ram[offs + 2 * interleave];
//     flipx = sprite_ram[offs] & 0x04;  flipy = sprite_ram[offs] & 0x02;
//     if (flip_screen()) { x = 240 - x; y = 240 - y + 0;
//                          flipx = !flipx; flipy = !flipy; }
//     y = y - 1;
//     ... draw at (x, y) ...
//     y = y + (flip_screen() ? -256 : 256);
//     ... draw again (wrap around) ...
//
// BOTH coordinates are subtractive, the Y adjust of 1 is this game's own
// (Eggs, on the same code path, passes 0), and every sprite is drawn TWICE
// to reproduce the hardware's wraparound. With x = 240 - value, a value
// above 239 gives a negative coordinate, which is exactly when the wrap
// copy becomes the visible one.
BTIME_VRAMFUNC static void draw_sprite_row_at(uint16_t code, int x, int sub_row,
                               bool flipx) {
    // Sprite row extraction, from tile16layout: 16x16, 3 planes at
    // RGN_FRAC{2,1,0}/3, xoffs { STEP8(16*8,1), STEP8(0,1) },
    // yoffs STEP16(0,8), 32 bytes per tile. Working the bit offsets through:
    //     x  0..7  <- byte [plane + code*32 + 16 + row], bits 7..0
    //     x  8..15 <- byte [plane + code*32 +      row], bits 7..0
    const uint32_t tile = code * 32u + (uint32_t)sub_row;
    const uint8_t *lo = &btime_gfx1[tile + 16u]; // left half  (x 0..7)
    const uint8_t *hi = &btime_gfx1[tile];       // right half (x 8..15)

    const uint8_t *l0 = spread[lo[GFX1_PLANE0]];
    const uint8_t *l1 = spread[lo[GFX1_PLANE1]];
    const uint8_t *l2 = spread[lo[GFX1_PLANE2]];
    const uint8_t *h0 = spread[hi[GFX1_PLANE0]];
    const uint8_t *h1 = spread[hi[GFX1_PLANE1]];
    const uint8_t *h2 = spread[hi[GFX1_PLANE2]];

    for (uint32_t j = 0; j < 16; j++) {
        uint8_t pen;
        if (j < 8) pen = (uint8_t)((l2[j] << 2) | (l1[j] << 1) | l0[j]);
        else {
            const uint32_t k = j - 8u;
            pen = (uint8_t)((h2[k] << 2) | (h1[k] << 1) | h0[k]);
        }
        if (pen == 0) continue; // transpen 0

        const int px = x + (int)(flipx ? (15u - j) : j);
        if (px < 0 || px >= BTIME_RASTER) continue;
        pen_row[px] = pen;
    }
}

BTIME_VRAMFUNC static void draw_sprites_row(const btime_system *s, uint32_t raw_y) {
    const bool flip = s->flip_screen;

    for (int i = 0; i < 8; i++) {
        const uint16_t offs = (uint16_t)(i * 4 * 0x20);
        const uint8_t attr = s->videoram[offs];
        if (!(attr & 0x01)) continue;

        int x = 240 - s->videoram[offs + 3 * 0x20];
        int y = 240 - s->videoram[offs + 2 * 0x20];
        bool flipx = (attr & 0x04) != 0;
        bool flipy = (attr & 0x02) != 0;

        if (flip) {
            x = 240 - x;
            y = 240 - y; // + sprite_y_adjust_flip_screen, which is 0 here
            flipx = !flipx;
            flipy = !flipy;
        }
        y = y - 1; // sprite_y_adjust

        const uint16_t code = s->videoram[offs + 0x20];

        // The sprite itself, then the wraparound copy.
        for (int pass = 0; pass < 2; pass++) {
            const int sy = (pass == 0) ? y : y + (flip ? -256 : 256);
            const int rel = (int)raw_y - sy;
            if (rel < 0 || rel > 15) continue;
            const int sub_row = flipy ? (15 - rel) : rel;
            draw_sprite_row_at(code, x, sub_row, flipx);
        }
    }
}

// draw_background(), btime.cpp:728, plus the page-list construction in
// screen_update_btime():
//
//     start = flip_screen() ? 0 : 1;
//     for (i = 0; i < 4; i++) { tilemap[i] = start | (bnj_scroll[0] & 0x04);
//                               start = (start + 1) & 0x03; }
//     scroll = -(bnj_scroll[1] | ((bnj_scroll[0] & 0x03) << 8));
//     for (i = 0; i < 5; i++, scroll += 256) {
//         tileoffset = tmap[i & 3] * 0x100;
//         if (scroll > 256) break;   if (scroll < -256) continue;
//         for (offs = 0; offs < 0x100; offs++) {
//             x = 240 - (16 * (offs / 16) + scroll) - 1;
//             y = 16 * (offs % 16);
//             if (flip_screen()) { x = 240 - x; y = 240 - y; }
//             gfx(2)->opaque(m_bg_map[tileoffset + offs], color, flip, flip, x, y);
//         }
//     }
//
// bnj_scroll[1] is permanently 0 on this game (btime_map() only wires
// bnj_scroll_w<0>, at 0x4004), so the scroll is one of 0, -256, -512, -768:
// a coarse 256-pixel offset, combined with the rotating page list, is how
// the layer is positioned. bg_map (ab03.6b, 2KB) is 8 pages of 256 tile
// numbers, each page a 16x16 arrangement of 16x16-pixel tiles = one 256x256
// screen.
//
// Row-oriented: y depends only on (offs % 16), so exactly 16 of a page's
// 256 tiles touch any given row -- iterate the 16 columns rather than all
// 256 cells and test.
BTIME_VRAMFUNC static void draw_background_row(const btime_system *s, uint32_t raw_y) {
    const bool flip = s->flip_screen;

    uint8_t tmap[4];
    {
        uint8_t start = flip ? 0u : 1u;
        for (int i = 0; i < 4; i++) {
            tmap[i] = (uint8_t)(start | (s->bnj_scroll0 & 0x04));
            start = (uint8_t)((start + 1) & 0x03);
        }
    }

    // Which (offs % 16) lands on this row, and which tile row shows there.
    const uint32_t row16 = raw_y >> 4;
    const uint32_t m       = flip ? (15u - row16) : row16;
    const uint32_t sub     = raw_y & 15u;
    const uint32_t tile_row = flip ? (15u - sub) : sub;

    int scroll = -(int)((s->bnj_scroll0 & 0x03u) << 8);
    for (int i = 0; i < 5; i++, scroll += 256) {
        if (scroll > 256) break;
        if (scroll < -256) continue;
        const uint32_t tileoffset = (uint32_t)tmap[i & 3] * 0x100u;

        for (uint32_t col = 0; col < 16; col++) {
            const uint32_t offs = col * 16u + m;
            // MAME's x is 240 - (16 * (offs / 16) + scroll) - 1, and with
            // offs = col*16 + m the (offs / 16) term is exactly col.
            int x = 240 - (int)(16u * col) - scroll - 1;
            if (flip) x = 240 - x;

            const uint8_t code = btime_bg_map[tileoffset + offs] & 0x3F;
            const uint8_t *src = bg_px[code][tile_row];

            // Clip the 16-pixel span ONCE rather than bounds-checking every
            // pixel: most tiles are fully on-raster, and the two at the
            // edges are the only ones that need trimming.
            if (!flip) {
                int j0 = 0, j1 = 16;
                if (x < 0) j0 = -x;
                if (x + 16 > BTIME_RASTER) j1 = BTIME_RASTER - x;
                // THE GUARD IS LOAD-BEARING. A tile can be entirely off the
                // raster (the page loop deliberately walks one copy past
                // each edge for wraparound), and then j1 < j0. The previous
                // per-pixel loop simply did not execute; a memcpy of
                // (j1 - j0) underflows size_t and segfaults instantly --
                // which is exactly what it did, on the first run after this
                // was optimised into a copy.
                if (j1 > j0)
                    // Opaque, and the pen base is already baked into the
                    // cache, so this is a plain copy.
                    memcpy(&pen_row[x + j0], &src[j0], (size_t)(j1 - j0));
            } else {
                for (uint32_t j = 0; j < 16; j++) {
                    const int px = x + (int)(15u - j);
                    if (px < 0 || px >= BTIME_RASTER) continue;
                    pen_row[px] = src[j];
                }
            }
        }
    }
}

// One raster row, in the hardware's own draw order.
BTIME_VRAMFUNC static void render_native_row(const btime_system *s, uint32_t raw_y) {
    if (s->bnj_scroll0 & 0x10) {
        // The background is opaque and covers the row, so no clear is
        // needed -- but only when it is actually enabled.
        draw_background_row(s, raw_y);
        draw_chars_row(s, raw_y, true);   // transparency = true
    } else {
        draw_chars_row(s, raw_y, false);  // opaque
    }
    draw_sprites_row(s, raw_y);
}

// --- column renderer -----------------------------------------------------
//
// The transpose of render_native_row(): given a raster COLUMN, produce the
// pens down it. Yoko needs this because there a physical scanline IS a
// raster column, and a row-only renderer cannot emit one until every row
// exists -- which is what forced the frame_pen cache and the whole-frame
// burst that starved the DVI queue (DEVNOTES #18/#79).
//
// Each layer keeps the structure of its row twin and swaps writing for
// testing: the same pages, cells and sprites are walked, but instead of
// writing a span, each asks "does this span cover raw_x" and then walks the
// OTHER axis. Burger Time has no per-scanline sprite arbitration (all 8 are
// drawn on every line), which is what makes a straight transposition
// possible at all.
static uint8_t pen_col[BTIME_RASTER];

// draw_chars_row()'s transpose. There the destination is cx*8 + j
// (unflipped) or cx*8 + 7 - j (flipped), so inverting for a fixed raw_x
// gives one cell column and one pixel column, then all 32 cell rows.
BTIME_VRAMFUNC static void draw_chars_col(const btime_system *s, uint32_t raw_x,
                                          bool transparent) {
    const bool flip = s->flip_screen;
    const uint32_t cx = raw_x >> 3;
    const uint32_t jj = raw_x & 7u;
    const uint32_t j  = flip ? (7u - jj) : jj;

    for (uint32_t cy = 0; cy < 32; cy++) {
        const uint16_t offs = flip ? (uint16_t)(32u * cx + (31u - cy))
                                   : (uint16_t)(32u * (31u - cx) + cy);
        const uint16_t code = (uint16_t)(s->videoram[offs] |
                                        ((s->colorram[offs] & 3u) << 8));

        // char_px_T is column-major, so the 8 pens down this cell are
        // CONTIGUOUS -- the same 8-byte copy draw_chars_row() makes, which
        // is the whole reason that cache exists.
        const uint8_t *src = char_px_T[code][j];
        uint8_t *out = &pen_col[cy * 8u];

        if (!transparent && !flip) {
            memcpy(out, src, 8);
        } else if (!flip) {
            for (uint32_t k = 0; k < 8; k++)
                if (src[k]) out[k] = src[k];
        } else {
            for (uint32_t k = 0; k < 8; k++) {
                const uint8_t pen = src[7u - k];
                if (transparent && pen == 0) continue;
                out[k] = pen;
            }
        }
    }
}

// draw_background_row()'s transpose. Same five pages and sixteen tile
// columns, but each is TESTED for covering raw_x rather than blitted; the
// one that does gives the pixel column `j`, and then all 16 row bands of
// 16 sub-rows are walked. Pages are processed in the same order so a later
// one still overwrites an earlier -- this layer is opaque.
BTIME_VRAMFUNC static void draw_background_col(const btime_system *s, uint32_t raw_x) {
    const bool flip = s->flip_screen;

    uint8_t tmap[4];
    {
        uint8_t start = flip ? 0u : 1u;
        for (int i = 0; i < 4; i++) {
            tmap[i] = (uint8_t)(start | (s->bnj_scroll0 & 0x04));
            start = (uint8_t)((start + 1) & 0x03);
        }
    }

    int scroll = -(int)((s->bnj_scroll0 & 0x03u) << 8);
    for (int i = 0; i < 5; i++, scroll += 256) {
        if (scroll > 256) break;
        if (scroll < -256) continue;
        const uint32_t tileoffset = (uint32_t)tmap[i & 3] * 0x100u;

        for (uint32_t col = 0; col < 16; col++) {
            int x = 240 - (int)(16u * col) - scroll - 1;
            if (flip) x = 240 - x;

            const int d = (int)raw_x - x;          // position within the tile
            if (d < 0 || d >= 16) continue;        // this tile misses the column
            const uint32_t j = flip ? (uint32_t)(15 - d) : (uint32_t)d;

            for (uint32_t row16 = 0; row16 < 16; row16++) {
                const uint32_t m    = flip ? (15u - row16) : row16;
                const uint32_t offs = col * 16u + m;
                const uint8_t code  = btime_bg_map[tileoffset + offs] & 0x3F;

                // Column-major, so this is the 16-byte copy
                // draw_background_row() makes. Opaque, pen base baked in.
                const uint8_t *src = bg_px_T[code][j];
                uint8_t *out = &pen_col[row16 * 16u];
                if (!flip) {
                    memcpy(out, src, 16);
                } else {
                    for (uint32_t sub = 0; sub < 16; sub++)
                        out[sub] = src[15u - sub];
                }
            }
        }
    }
}

// One pixel of a sprite, by (row, column) -- the per-pixel form of
// draw_sprite_row_at()'s bit-spread. Same plane layout and same
// left/right-half byte split.
BTIME_VRAMFUNC static inline uint8_t sprite_pixel(uint16_t code, int sub_row, uint32_t j) {
    const uint32_t tile = code * 32u + (uint32_t)sub_row;
    const uint8_t *b = (j < 8u) ? &btime_gfx1[tile + 16u] : &btime_gfx1[tile];
    const uint32_t k = (j < 8u) ? j : j - 8u;
    return (uint8_t)((spread[b[GFX1_PLANE2]][k] << 2) |
                     (spread[b[GFX1_PLANE1]][k] << 1) |
                      spread[b[GFX1_PLANE0]][k]);
}

// draw_sprites_row()'s transpose: the x overlap is tested once per sprite
// instead of per row, then all 16 rows of the matching column are written.
// Both wraparound passes are kept, in the same order.
BTIME_VRAMFUNC static void draw_sprites_col(const btime_system *s, uint32_t raw_x) {
    const bool flip = s->flip_screen;

    for (int i = 0; i < 8; i++) {
        const uint16_t offs = (uint16_t)(i * 4 * 0x20);
        const uint8_t attr = s->videoram[offs];
        if (!(attr & 0x01)) continue;

        int x = 240 - s->videoram[offs + 3 * 0x20];
        int y = 240 - s->videoram[offs + 2 * 0x20];
        bool flipx = (attr & 0x04) != 0;
        bool flipy = (attr & 0x02) != 0;

        if (flip) {
            x = 240 - x;
            y = 240 - y;
            flipx = !flipx;
            flipy = !flipy;
        }
        y = y - 1;

        const int d = (int)raw_x - x;
        if (d < 0 || d >= 16) continue;            // sprite misses this column
        const uint32_t j = flipx ? (uint32_t)(15 - d) : (uint32_t)d;

        const uint16_t code = s->videoram[offs + 0x20];

        for (int pass = 0; pass < 2; pass++) {
            const int sy = (pass == 0) ? y : y + (flip ? -256 : 256);
            for (int rel = 0; rel < 16; rel++) {
                const int ry = sy + rel;
                if (ry < 0 || ry >= BTIME_RASTER) continue;
                const int sub_row = flipy ? (15 - rel) : rel;
                const uint8_t pen = sprite_pixel(code, sub_row, j);
                if (pen == 0) continue;            // transpen 0
                pen_col[ry] = pen;
            }
        }
    }
}

// One raster column, in the hardware's own draw order -- the same order
// render_native_row() uses.
BTIME_VRAMFUNC static void render_native_column(const btime_system *s, uint32_t raw_x) {
    if (s->bnj_scroll0 & 0x10) {
        draw_background_col(s, raw_x);
        draw_chars_col(s, raw_x, true);   // transparency = true
    } else {
        draw_chars_col(s, raw_x, false);  // opaque
    }
    draw_sprites_col(s, raw_x);
}

// --- output --------------------------------------------------------------

// THE frame_pen CACHE THAT USED TO LIVE HERE IS GONE. It held the whole
// frame as 8-bit pens (57.6KB) purely so landscape/180 could emit a raster
// column, and building it was a whole-frame burst before the first
// hal_video_acquire_scanline() -- DEVNOTES #18, and #75 for the
// measurement. render_native_column() above removed the reason for it.

// Writes one tate scanline: `row` is the visible raster row (0..239) and
// `reverse` mirrors the output along the buffer, which is what separates
// 90 CCW from 90 CW.
// Writes ALL HAL_VIDEO_WIDTH canvas columns, pillarbox included, so that
// btime_video_render_scanline() does not have to pre-clear the buffer.
// That clear was not free: back when HAL_VIDEO_WIDTH was the PHYSICAL 640
// rather than the 320-pixel canvas, memset()ing it on every one of 240
// scanlines was 307KB of stores per frame, most of it immediately
// overwritten and half of it into columns the display never reads. This
// file stopping at 320 by hand is what the rest of the project now gets
// for free from the corrected HAL geometry (DISPLAY_GEOMETRY.md phase 1).

// Emits one yoko scanline straight out of pen_col: palette lookup, canvas
// mapping and border clear in a single pass.
//
// The obvious composition -- convert pen_col to RGB, then call
// av_emit_row_merge() -- costs an extra full pass over the column plus a
// 320-pixel clear of the whole scanline, and this game has ~1.1ms of
// headroom to spend (DEVNOTES #81). Only the borders need clearing here
// because every canvas column inside [x0,x1) is written unconditionally by
// the rep >= 1 case.
//
// The rep == 0 case is the merge: that raster sample was collapsed away by
// the aspect correction's 240 -> 180 narrowing, and a non-background pen
// must not be lost to it (DEVNOTES #80). `pen` is the game's own
// transparency, which is what makes this test meaningful rather than a
// comparison against black.
BTIME_VRAMFUNC static void emit_yoko_col(uint16_t *buf, bool reverse) {
    const uint8_t *vis = &pen_col[BTIME_FIRST_VISIBLE_LINE];
    const uint8_t *rep = av_yoko.rep;
    const uint32_t n    = av_yoko.src_n;
    const uint32_t x0   = av_yoko.x0;
    const uint32_t x1   = av_yoko.x1;
    const uint32_t last = n - 1u;

    // 1:1 FAST PATH -- the aspect correction is off on this game (see
    // btime_machine.cpp), so this is what actually ships. Two pixels per
    // 32-bit store, unrolled by four, exactly as emit_tate_row() does:
    // BTIME_GAME_HEIGHT is 240 and x0 is 40, so `buf + x0` is 4-byte
    // aligned and 240 divides by 4. Without this, yoko cost ~400us a frame
    // more than tate for identical output, which on this game's headroom
    // was the difference between clean and red lines in the busy attract
    // scenes (DEVNOTES #81).
    if (av_yoko.col_1to1) {
        uint32_t *out32 = (uint32_t *)(buf + x0);
        if (!reverse) {
            for (uint32_t i = 0; i < (uint32_t)BTIME_GAME_HEIGHT / 4u; i++) {
                const uint32_t c = i * 4u;
                out32[i * 2u + 0] = (uint32_t)pal565[vis[c + 0]] |
                                    ((uint32_t)pal565[vis[c + 1]] << 16);
                out32[i * 2u + 1] = (uint32_t)pal565[vis[c + 2]] |
                                    ((uint32_t)pal565[vis[c + 3]] << 16);
            }
        } else {
            for (uint32_t i = 0; i < (uint32_t)BTIME_GAME_HEIGHT / 4u; i++) {
                const int c = (int)(i * 4u);
                out32[i * 2u + 0] = (uint32_t)pal565[vis[last - (uint32_t)c]] |
                                    ((uint32_t)pal565[vis[last - (uint32_t)(c + 1)]] << 16);
                out32[i * 2u + 1] = (uint32_t)pal565[vis[last - (uint32_t)(c + 2)]] |
                                    ((uint32_t)pal565[vis[last - (uint32_t)(c + 3)]] << 16);
            }
        }
        for (uint32_t i = 0;  i < x0;              i++) buf[i] = 0;
        for (uint32_t i = x1; i < HAL_VIDEO_WIDTH; i++) buf[i] = 0;
        return;
    }

    uint32_t x = x0;
    for (uint32_t s = 0; s < n; s++) {
        const uint8_t pen = reverse ? vis[last - s] : vis[s];
        const uint32_t r = rep[s];
        if (r) { buf[x] = pal565[pen]; x += r; }
        else if (pen) { buf[x] = pal565[pen]; }
    }

    for (uint32_t i = 0;  i < x0;              i++) buf[i] = 0;
    for (uint32_t i = x1; i < HAL_VIDEO_WIDTH; i++) buf[i] = 0;
}

BTIME_VRAMFUNC static void emit_tate_row(uint16_t *buf, bool reverse) {
    const uint8_t *vis = &pen_row[BTIME_FIRST_VISIBLE_LINE]; // raw x 8..247

    // The `reverse` test is hoisted OUT of these loops rather than
    // evaluated per pixel, and the pen values are used unmasked: every
    // writer into pen_row produces 0-7 (chars, sprites) or 8-15 (the
    // background cache, base baked in), so the values are already in range
    // and an `& 0x0F` per pixel would be 320 wasted operations per scanline.
    if (!av_tate.col_1to1) {
        // Aspect-corrected: the raster's 240 columns spread over all 320
        // canvas columns. SOURCE-driven, not destination-driven -- walking
        // the 320 outputs and indexing back through av_tate.col[] is a
        // dependent load pair per pixel and measured 2.3ms a frame more
        // than the 1:1 path, which this game cannot afford (DEVNOTES #81).
        // Here each of the 240 raster samples is read once and written to
        // its one or two canvas columns.
        //
        // The second store is what removes the branch: a sample covering
        // one column writes one pixel too far and the next iteration
        // overwrites it. The final overspill lands at x1, which the border
        // clear below mops up.
        const uint8_t *rep  = av_tate.rep;
        const uint32_t n    = av_tate.src_n;
        const uint32_t last = n - 1u;
        uint32_t x = av_tate.x0;

        if (!reverse) {
            for (uint32_t s = 0; s < n; s++) {
                const uint16_t v = pal565[vis[s]];
                buf[x] = v; buf[x + 1] = v;
                x += rep[s];
            }
        } else {
            for (uint32_t s = 0; s < n; s++) {
                const uint16_t v = pal565[vis[last - s]];
                buf[x] = v; buf[x + 1] = v;
                x += rep[s];
            }
        }

        for (uint32_t i = 0; i < av_tate.x0; i++) buf[i] = 0;
        for (uint32_t i = av_tate.x1; i < HAL_VIDEO_WIDTH; i++) buf[i] = 0;
        return;
    }

    for (uint32_t col = 0; col < av_tate.x0; col++) buf[col] = 0; // left bar
    uint16_t *out = buf + av_tate.x0;
    if (!reverse) {
        // Two pixels per 32-bit store. av_tate.x0 is 40 here and the buffer
        // is 16-bit, so `out` is 4-byte aligned and this is safe; it halves
        // the store count into the DVI scanline buffer, which matters more
        // than it looks because Core 1's DVI DMA is hammering the same
        // SRAM. Unrolled by 4 pixels (240 divides exactly) to cut loop
        // overhead as well. Only valid while the column map is an identity
        // shift, which is what the branch above tests.
        uint32_t *out32 = (uint32_t *)out;
        for (uint32_t i = 0; i < BTIME_GAME_WIDTH / 4u; i++) {
            const uint32_t c = i * 4u;
            out32[i * 2u + 0] = (uint32_t)pal565[vis[c + 0]] |
                                ((uint32_t)pal565[vis[c + 1]] << 16);
            out32[i * 2u + 1] = (uint32_t)pal565[vis[c + 2]] |
                                ((uint32_t)pal565[vis[c + 3]] << 16);
        }
    } else {
        // The SAME two-pixels-per-32-bit-store trick as the forward path.
        // It used to be a plain per-pixel loop, which made rotation 3 cost
        // ~600us a frame more than rotation 1 for identical output -- and on
        // this game's ~1.1ms of headroom that was the difference between
        // clean and red lines during the busy attract scenes (DEVNOTES #81).
        // A reversed read is still a sequential read; there was never a
        // reason for the slow path.
        uint32_t *out32 = (uint32_t *)out;
        const uint8_t *rev = vis + BTIME_GAME_WIDTH - 1u;
        for (uint32_t i = 0; i < BTIME_GAME_WIDTH / 4u; i++) {
            const int c = (int)(i * 4u);
            out32[i * 2u + 0] = (uint32_t)pal565[rev[-c]] |
                                ((uint32_t)pal565[rev[-(c + 1)]] << 16);
            out32[i * 2u + 1] = (uint32_t)pal565[rev[-(c + 2)]] |
                                ((uint32_t)pal565[rev[-(c + 3)]] << 16);
        }
    }
    for (uint32_t col = av_tate.x1; col < HAL_VIDEO_WIDTH; col++)
        buf[col] = 0; // right bar
}

BTIME_VRAMFUNC void btime_video_render_scanline(const btime_system *s, uint32_t dvi_y,
                                 uint16_t *buf) {
    const bool mir = s->mirror_x;

    switch (s->rotation) {

    case 0: {
        // Landscape: this scanline IS a raster column. av_yoko.row picks
        // which one -- 1:1 here, since this game's long axis is 240 and so
        // are the canvas rows, the only game in the project where that
        // holds -- and av_yoko.col maps canvas columns onto the short axis.
        //
        // The `(W-1) - dy` reversal is NOT cosmetic: a 90-degree rotation
        // must reverse exactly one axis relative to tate (case 1, which
        // does not), or the result is a mirror image (DEVNOTES #21).
        render_native_column(s, BTIME_FIRST_VISIBLE_LINE +
                                (uint32_t)(BTIME_GAME_WIDTH - 1) - av_yoko.row[dvi_y]);
        emit_yoko_col(buf, mir);
        break;
    }

    case 1: {
        // 90 deg CCW (tate, the default). Burger Time's raster is 240 rows
        // against 240 canvas rows, so av_tate.row is 1:1 here with no
        // letterbox -- the only game in the project where that is true.
        if (dvi_y < av_tate.y0 || dvi_y >= av_tate.y1) {
            memset(buf, 0, HAL_VIDEO_WIDTH * sizeof(uint16_t));
            return;
        }
        uint32_t row = av_tate.row[dvi_y];
        if (mir) row = (uint32_t)(BTIME_GAME_HEIGHT - 1) - row;
        render_native_row(s, BTIME_FIRST_VISIBLE_LINE + row);
        emit_tate_row(buf, false);
        break;
    }

    case 2: {
        // 180 deg: case 0 with BOTH axes reversed relative to it, so the
        // column index is deliberately the *un*-reversed raw value and the
        // emit direction flips.
        render_native_column(s, BTIME_FIRST_VISIBLE_LINE + av_yoko.row[dvi_y]);
        emit_yoko_col(buf, !mir);
        break;
    }

    case 3: {
        // 90 deg CW: the other tate, reversing both the scanline order and
        // the within-scanline order relative to case 1.
        if (dvi_y < av_tate.y0 || dvi_y >= av_tate.y1) {
            memset(buf, 0, HAL_VIDEO_WIDTH * sizeof(uint16_t));
            return;
        }
        const uint32_t r   = av_tate.row[dvi_y];
        const uint32_t row = mir ? r : (uint32_t)(BTIME_GAME_HEIGHT - 1) - r;
        render_native_row(s, BTIME_FIRST_VISIBLE_LINE + row);
        emit_tate_row(buf, true);
        break;
    }

    default: break;
    }
}

void btime_draw_error_frame(uint16_t color) {
    for (uint32_t i = 0; i < HAL_VIDEO_HEIGHT; i++) {
        uint16_t *buf = hal_video_acquire_scanline();
        for (uint32_t x = 0; x < HAL_VIDEO_WIDTH; x++) buf[x] = color;
        hal_video_submit_scanline(buf);
    }
}
