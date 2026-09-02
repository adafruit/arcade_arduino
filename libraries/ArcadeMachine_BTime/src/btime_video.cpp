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
// NO DECODE CACHES, WHICH IS A DELIBERATE DEPARTURE from
// ArcadeMachine_Pacman/DKong. Those games' graphics ROMs pack pixels in
// awkward interleaved nibbles, so decoding once into a pixel cache is a
// clear win. Burger Time's are straight bitplanes (three parallel copies of
// the same shape, one bit per pixel each), so extracting one ROW of one
// character is three byte reads plus a bit-spread -- cheap enough to do on
// demand, every frame. What that buys:
//
//     char pixel cache  [1024][8][8]  would be  64 KB
//     sprite cache      [256][16][16] would be  64 KB
//     this file's bit-spread table    is         2 KB
//
// The renderer is row-oriented for the same reason the other tate renderers
// are: one submitted DVI scanline is one raster row, rendered on demand from
// live VRAM, interleaved with CPU execution (DEVNOTES.md #19/#20/#34/#36).
// If `work` ever needs the headroom, a char pixel cache is the obvious
// lever -- but measure first (#35), because this is not the expensive part.
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
#define TATE_BY  0u    // (480 - 240*2) / 2 -- fills every scanline, 1:1
#define TATE_BX  40u   // (320 - 240)   / 2 -- centred, 40px pillarbox
#define LAND_BX  40u   // (320 - 240)   / 2

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
static bool g_aspect_stretch = false;

void btime_video_set_aspect_stretch(bool enable) { g_aspect_stretch = enable; }
bool btime_video_get_aspect_stretch(void) { return g_aspect_stretch; }

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

void btime_video_build_caches(void) {
    for (int b = 0; b < 256; b++)
        for (int j = 0; j < 8; j++)
            spread[b][j] = (uint8_t)((b >> (7 - j)) & 1);

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
static void draw_chars_row(const btime_system *s, uint32_t raw_y,
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

        const uint8_t *base = &btime_gfx1[code * 8u + char_row];
        const uint8_t *s0 = spread[base[GFX1_PLANE0]];
        const uint8_t *s1 = spread[base[GFX1_PLANE1]];
        const uint8_t *s2 = spread[base[GFX1_PLANE2]];

        uint8_t *out = &pen_row[cx * 8u];
        for (uint32_t j = 0; j < 8; j++) {
            const uint8_t pen = (uint8_t)((s2[j] << 2) | (s1[j] << 1) | s0[j]);
            const uint32_t px = flip ? (7u - j) : j;
            if (transparent && pen == 0) continue;
            out[px] = pen;
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
static void draw_sprite_row_at(uint16_t code, int x, int sub_row,
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

static void draw_sprites_row(const btime_system *s, uint32_t raw_y) {
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
static void draw_background_row(const btime_system *s, uint32_t raw_y) {
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

            const uint16_t code = btime_bg_map[tileoffset + offs];
            const uint32_t tile = code * 32u + tile_row;
            const uint8_t *lo = &btime_gfx2[tile + 16u]; // x 0..7
            const uint8_t *hi = &btime_gfx2[tile];       // x 8..15

            const uint8_t *l0 = spread[lo[GFX2_PLANE0]];
            const uint8_t *l1 = spread[lo[GFX2_PLANE1]];
            const uint8_t *l2 = spread[lo[GFX2_PLANE2]];
            const uint8_t *h0 = spread[hi[GFX2_PLANE0]];
            const uint8_t *h1 = spread[hi[GFX2_PLANE1]];
            const uint8_t *h2 = spread[hi[GFX2_PLANE2]];

            for (uint32_t j = 0; j < 16; j++) {
                uint8_t pixel;
                if (j < 8) pixel = (uint8_t)((l2[j] << 2) | (l1[j] << 1) | l0[j]);
                else {
                    const uint32_t k = j - 8u;
                    pixel = (uint8_t)((h2[k] << 2) | (h1[k] << 1) | h0[k]);
                }
                const int px = x + (int)(flip ? (15u - j) : j);
                if (px < 0 || px >= BTIME_RASTER) continue;
                pen_row[px] = (uint8_t)(BG_PEN_BASE + pixel); // opaque
            }
        }
    }
}

// One raster row, in the hardware's own draw order.
static void render_native_row(const btime_system *s, uint32_t raw_y) {
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

// --- output --------------------------------------------------------------

// Landscape/180 need the whole frame before any physical scanline can be
// emitted (each of those scanlines is a raster COLUMN), so those two modes
// keep the fully-sequential path. Stored as 8-bit pens rather than RGB565:
// 57.6KB instead of 115KB, and the palette lookup is per output pixel
// either way. Same known, deprioritised limitation ArcadeMachine_Pacman has
// in those orientations (DEVNOTES.md #19).
static uint8_t frame_pen[BTIME_GAME_HEIGHT][BTIME_GAME_WIDTH];

// Writes one tate scanline: `row` is the visible raster row (0..239) and
// `reverse` mirrors the output along the buffer, which is what separates
// 90 CCW from 90 CW.
static void emit_tate_row(uint16_t *buf, bool reverse) {
    const uint8_t *vis = &pen_row[BTIME_FIRST_VISIBLE_LINE]; // raw x 8..247

    if (g_aspect_stretch) {
        // Spread 240 raster columns over all 320 framebuffer columns; see
        // this file's geometry comment. col * 3 / 4 is exact for 320 -> 240.
        for (uint32_t col = 0; col < 320u; col++) {
            const uint32_t nx = (col * 3u) >> 2;
            const uint32_t src = reverse ? (BTIME_GAME_WIDTH - 1u - nx) : nx;
            buf[col] = pal565[vis[src] & 0x0Fu];
        }
    } else {
        for (uint32_t col = 0; col < BTIME_GAME_WIDTH; col++) {
            const uint32_t src = reverse ? (BTIME_GAME_WIDTH - 1u - col) : col;
            buf[TATE_BX + col] = pal565[vis[src] & 0x0Fu];
        }
    }
}

void btime_video_render_scanline(const btime_system *s, uint32_t dvi_y,
                                 uint16_t *buf) {
    memset(buf, 0, HAL_VIDEO_WIDTH * sizeof(uint16_t));
    const bool mir = s->mirror_x;

    switch (s->rotation) {

    case 0: {
        // Landscape: the raster's horizontal axis (240 columns) stretches
        // across all 480 DVI scanlines; its vertical axis fills the
        // 320-wide window 1:1. The `(W-1) - dy` reversal is NOT cosmetic --
        // a 90-degree rotation must reverse exactly one axis relative to
        // tate (case 1, which does not reverse its equivalent), or the
        // result is a mirror image rather than a rotation. Same shape as
        // invaders_video.cpp's case 0, which is the real-hardware-verified
        // original.
        const uint32_t dy = (dvi_y * (uint32_t)BTIME_GAME_WIDTH) / HAL_VIDEO_HEIGHT;
        const uint32_t col = (uint32_t)(BTIME_GAME_WIDTH - 1) - dy;
        for (uint32_t i = 0; i < (uint32_t)BTIME_GAME_HEIGHT; i++) {
            const uint32_t dx = mir ? (uint32_t)(BTIME_GAME_HEIGHT - 1) - i : i;
            buf[LAND_BX + i] = pal565[frame_pen[dx][col] & 0x0Fu];
        }
        break;
    }

    case 1: {
        // 90 deg CCW (tate, the default). TATE_BY is 0 and the raster is
        // 240 rows tall against 240 submitted scanlines, so this is 1:1
        // with no border and no clipping -- the only game here where that
        // is true.
        if (dvi_y >= HAL_VIDEO_HEIGHT) return;
        uint32_t row = dvi_y >> 1;
        if (mir) row = (uint32_t)(BTIME_GAME_HEIGHT - 1) - row;
        render_native_row(s, BTIME_FIRST_VISIBLE_LINE + row);
        emit_tate_row(buf, false);
        break;
    }

    case 2: {
        // 180 deg: case 0 with both axes reversed relative to it, so `col`
        // is deliberately the un-reversed raw value and `dx`'s ternary is
        // the mirror of case 0's -- matching invaders_video.cpp's case 2.
        const uint32_t col = (dvi_y * (uint32_t)BTIME_GAME_WIDTH) / HAL_VIDEO_HEIGHT;
        for (uint32_t i = 0; i < (uint32_t)BTIME_GAME_HEIGHT; i++) {
            const uint32_t dx = mir ? i : (uint32_t)(BTIME_GAME_HEIGHT - 1) - i;
            buf[LAND_BX + i] = pal565[frame_pen[dx][col] & 0x0Fu];
        }
        break;
    }

    case 3: {
        // 90 deg CW: the other tate, reversing both the scanline order and
        // the within-scanline order relative to case 1.
        if (dvi_y >= HAL_VIDEO_HEIGHT) return;
        const uint32_t r = dvi_y >> 1;
        const uint32_t row = mir ? r : (uint32_t)(BTIME_GAME_HEIGHT - 1) - r;
        render_native_row(s, BTIME_FIRST_VISIBLE_LINE + row);
        emit_tate_row(buf, true);
        break;
    }

    default: break;
    }
}

void btime_draw_frame(btime_system *system) {
    // Only landscape/180 need the frame buffer; tate renders on demand
    // inside btime_run_frame()'s scanline loop.
    if (system->rotation == 0 || system->rotation == 2) {
        for (uint32_t row = 0; row < (uint32_t)BTIME_GAME_HEIGHT; row++) {
            render_native_row(system, BTIME_FIRST_VISIBLE_LINE + row);
            memcpy(frame_pen[row], &pen_row[BTIME_FIRST_VISIBLE_LINE],
                   BTIME_GAME_WIDTH);
        }
    }

    const uint32_t step = HAL_VIDEO_HEIGHT / HAL_VIDEO_SCANLINES_PER_FRAME;
    for (uint32_t i = 0; i < HAL_VIDEO_SCANLINES_PER_FRAME; i++) {
        uint16_t *buf = hal_video_acquire_scanline();
        btime_video_render_scanline(system, i * step, buf);
        hal_video_submit_scanline(buf);
    }
}

void btime_draw_error_frame(uint16_t color) {
    for (uint32_t i = 0; i < HAL_VIDEO_SCANLINES_PER_FRAME; i++) {
        uint16_t *buf = hal_video_acquire_scanline();
        for (uint32_t x = 0; x < HAL_VIDEO_WIDTH; x++) buf[x] = color;
        hal_video_submit_scanline(buf);
    }
}
