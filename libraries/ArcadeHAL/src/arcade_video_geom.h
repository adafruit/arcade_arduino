// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Shared screen geometry: where a game's raster lands on the canvas, and
// how it is resampled to get there. One source of truth for all seven
// machines. See DISPLAY_GEOMETRY.md for the derivation.
//
// WHAT THIS REPLACES. Every renderer carried its own hand-derived
// TATE_BX/TATE_BY/LAND_BX constants and its own inline resampling
// arithmetic, four rotation cases at a time. That is 32 hand-derived cases
// across the project, each verified separately, and it is what produced
// DEVNOTES #21 (a mirror instead of a rotation), #33 (a default rotation
// with no fast path) and #23 (the landscape resampling). All three are the
// same failure: a case derived from a sibling case rather than from one
// source of truth.
//
// THE GEOMETRY, in one place.
//
// The canvas is 320x240 logical SQUARE pixels on a 4:3 panel
// (arcade_hal_video.h). Name a game's raster by its axes rather than by
// "width" and "height", which are ambiguous once a cabinet is rotated:
//
//   LONG  axis -- the raster axis that runs VERTICALLY on a real rotated
//                 cabinet's tube. 256 for Invaders/Lunar Rescue/DKong, 288
//                 for the Namco games, 240 for Burger Time. This is the
//                 dimension those machines call *_GAME_WIDTH.
//   SHORT axis -- the other one. 224, or 240 for Burger Time.
//                 Called *_GAME_HEIGHT.
//
// A real cabinet fills its tube, so the displayed picture is 4 units along
// LONG by 3 along SHORT. The canvas is 4 units by 3. Therefore:
//
//   TATE (monitor physically rotated): LONG -> all 320 canvas columns,
//        SHORT -> all 240 canvas rows. The picture fills the screen.
//   YOKO (monitor upright, portrait game shown inside it): LONG is capped
//        by the 240 canvas rows, so SHORT must be 240 * 3/4 = 180 columns,
//        pillarboxed 70 each side.
//
// **BOTH DESTINATIONS ARE THE SAME FOR EVERY GAME** -- 320x240 and 180x240.
// Only the source raster differs, so only the resampling ratio is
// per-game. That is why this can be one module rather than seven.
//
// YOKO NEEDS THE PICTURE NARROWER, NOT TALLER. This is the one most likely
// to be got backwards: the old code drew SHORT at 1:1 (224 columns) where
// 180 is correct, which is the whole of landscape's 24.4% error.
//
// COST. Resampling is a table lookup per pixel, built once at init. No
// division and no branch in any inner loop -- which matters, because Donkey
// Kong runs at ~15.5ms of a 16.66ms budget and tate's 224->240 upsample
// makes 16 canvas rows repeat a source row (see av_map_t::row and the
// memoisation note on it).
#ifndef ARCADE_VIDEO_GEOM_H
#define ARCADE_VIDEO_GEOM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Array bounds. These are the canvas size this module is compiled for; a
// board with a larger canvas needs them raised. av_geom_init() clamps to
// the board's actual HAL_VIDEO_WIDTH/HEIGHT at runtime.
#define AV_CANVAS_W 320u
#define AV_CANVAS_H 240u

// Aspect-correct SHORT-axis extent in yoko: 240 * 3/4. Independent of the
// game -- see this file's header.
#define AV_YOKO_W   180u

// One orientation's mapping from canvas coordinates to raster indices.
//
// `col`/`row` are only meaningful inside [x0,x1) and [y0,y1); outside those
// the canvas is border and the renderer clears it. Keeping the borders as
// RANGES rather than as a sentinel inside the tables is deliberate: a
// sentinel would put a branch in a loop that runs 76,800 times a frame.
typedef struct {
    uint16_t x0, x1;            // canvas columns carrying picture, [x0, x1)
    uint16_t y0, y1;            // canvas rows carrying picture,    [y0, y1)
    uint16_t col[AV_CANVAS_W];  // canvas x -> raster index along one axis
    uint16_t row[AV_CANVAS_H];  // canvas y -> raster index along the other

    // True when `col` is the identity shift, i.e. col[x0 + i] == i. A
    // renderer that can write its raster row STRAIGHT into the scanline
    // buffer (galaga_video.cpp's tate fast path does exactly this) may take
    // that shortcut only when this is set; otherwise it must render to a
    // scratch row and map through `col`.
    //
    // This is a real performance fork, not a tidiness one. DEVNOTES #33:
    // when Galaga's default rotation changed to one WITHOUT a direct path,
    // the extra clear-and-copy per scanline was instantly enough to blow
    // its ~3ms of headroom and put red bars back on a real screen. Galaga
    // peaks at 14946us of a 16660us budget, so the shortcut is load-bearing
    // at that game's default.
    uint8_t  col_1to1;

    // The SAME column mapping, inverted: rep[s] is how many canvas columns
    // raster sample `s` covers, and src_n is how many samples `col` draws
    // from. Sum(rep[0..src_n-1]) == x1 - x0.
    //
    // This exists because `buf[x] = row[col[x]]` -- the obvious way to use
    // `col` -- measured badly. It is a dependent load pair (fetch the index,
    // then fetch the pixel) plus a store, 320 times a scanline, 240 times a
    // frame, on the SRAM the DVI DMA is already hammering. Walking the
    // SOURCE instead turns that into one load and two stores with nothing
    // dependent, which is what av_emit_row() below does. See DEVNOTES #78:
    // routing the unstretched path through `col` cost Donkey Kong 1.6ms a
    // frame without changing a pixel.
    uint16_t src_n;
    uint8_t  rep[AV_CANVAS_W];

    // The same inversion for the ROW axis: rowrep[y] is how many raster
    // samples collapse into canvas row y. 1 when the axis is 1:1 or being
    // upsampled; 1 or 2 where it is being downsampled.
    //
    // Only yoko downsamples its row axis (288 raster samples onto 240 canvas
    // rows), and only a renderer that produces one raster line per canvas
    // row needs this -- to render the collapsed lines too and merge them,
    // instead of letting nearest-neighbour delete them outright. Measured on
    // Pac-Man: sampling one line per canvas row keeps 58.2% of the lit
    // pixels, merging both axes keeps 82.3%.
    uint8_t  rowrep[AV_CANVAS_H];

    // UPSAMPLE SHAPE, or 0 when this axis is not a uniform upsample.
    //
    // Every upsampling ratio in this project turns out to be the same
    // shape: "every k-th source pixel is doubled, and it is the FIRST of
    // each group of k".
    //
    //     240 -> 320  k=3      256 -> 320  k=4
    //     288 -> 320  k=9      224 -> 240  k=14
    //
    // That is not a coincidence -- each is n -> n*(k+1)/k, because the
    // canvas is 320 and the rasters are what they are. It is what makes a
    // WIDE-STORE emit possible: a group of k source pixels produces exactly
    // k+1 canvas pixels, so for odd k (and for two groups when k is even)
    // the group lands on a whole number of 32-bit words and can be written
    // two pixels at a time.
    //
    // Set only after VERIFYING rep[] actually has that shape, never inferred
    // from the ratio alone -- see build_dbl_k(). Only the values with a
    // wide-store loop below (3, 4, 9) are ever stored; anything else stays 0
    // and the callers fall back to the scalar emit.
    uint16_t dbl_k;
} av_map_t;

// Tate (rotations 1 and 3): col indexes the LONG axis, row the SHORT axis.
extern av_map_t av_tate;
// Yoko (rotations 0 and 2): col indexes the SHORT axis, row the LONG axis.
extern av_map_t av_yoko;

// Build both maps for a game's raster. Call once from the machine's init,
// before the first frame. `long_px` is *_GAME_WIDTH, `short_px` is
// *_GAME_HEIGHT.
void av_geom_init(uint32_t long_px, uint32_t short_px);

// Aspect-ratio correction on/off, rebuilding both maps. OFF reproduces the
// project's historical 1:1-plus-pillarbox layout EXACTLY, pixel for pixel,
// which is what makes an A/B on real hardware a single button press and
// what lets the host harness prove this module changed nothing when off.
//
// Default is OFF. Turning it on is a visual change to every game and should
// be compared on the physical display before it becomes the shipped
// default -- the same discipline btime_video.cpp already used for its own
// one-game version of this.
void av_geom_set_stretch(bool on);
bool av_geom_get_stretch(void);

// Writes one raster row onto the canvas through `m`'s column mapping.
// `dst` is the whole scanline buffer (indexed by absolute canvas x); `src`
// is `m->src_n` raster samples. Equivalent to
//
//     for (x = m->x0; x < m->x1; x++) dst[x] = src[m->col[x]];
//
// but source-driven, so the inner loop is one load, two stores and an add
// -- no dependent second load and no branch. See av_map_t::rep.
//
// The second store is deliberate and is what removes the branch: when a
// sample covers only one column it writes one pixel too far, and the next
// iteration overwrites it. The final iteration's overspill lands past the
// picture, which the two trailing stores below clear. Callers may therefore
// clear the borders either before or after this; the picture's own columns
// are always written.
static inline void av_emit_row(uint16_t *dst, const uint16_t *src,
                               const av_map_t *m) {
    // Hoist every field into a local FIRST. `dst` is uint16_t* and *m holds
    // uint16_t, so the compiler must assume a store through `dst` can alias
    // the map and reload m->src_n / m->rep on every iteration. Measured on
    // hardware: leaving them in place made this loop 5.4x a plain linear
    // copy (DEVNOTES #78).
    const uint8_t *rep = m->rep;
    const uint32_t n   = m->src_n;
    const uint32_t x1  = m->x1;
    uint32_t x = m->x0;
    for (uint32_t s = 0; s < n; s++) {
        const uint16_t v = src[s];
        dst[x]     = v;
        dst[x + 1] = v;
        x += rep[s];
    }
    dst[x1]      = 0; // overspill, always inside the buffer's slack
    dst[x1 + 1u] = 0;
}

// DOWNSAMPLING variants. Where av_emit_row() spreads each raster sample over
// one or two canvas columns, these collapse one or two samples into one
// column -- and the rule is that a NON-BACKGROUND sample never loses to a
// background one.
//
// That rule is the whole point. Nearest-neighbour picks one sample per
// canvas column and discards the rest, which on 1-pixel line art does not
// thin a maze wall, it DELETES it: 33 of Pac-Man's 224 raster columns and 21
// of its 288 rows contain picture and are never sampled at all. Merging
// keeps every feature, at the cost of thickening some by a pixel. See
// DEVNOTES #80.
//
// Requires `dst` to be pre-cleared, which every renderer here already does.
// There is no double store, so nothing spills into the next column.
static inline void av_emit_row_merge(uint16_t *dst, const uint16_t *src,
                                     const av_map_t *m) {
    const uint8_t *rep = m->rep;   // hoisted -- see av_emit_row()
    const uint32_t n   = m->src_n;
    uint32_t x = m->x0;
    for (uint32_t s = 0; s < n; s++) {
        const uint16_t v = src[s];
        if (v) dst[x] = v;
        x += rep[s];
    }
}

// av_emit_row_merge() mirrored. As in av_emit_row_rev(), `rep` is walked
// FORWARDS while `src` is walked backwards: only the pixel VALUES mirror,
// the column widths do not.
static inline void av_emit_row_merge_rev(uint16_t *dst, const uint16_t *src,
                                         const av_map_t *m) {
    const uint8_t *rep = m->rep;
    const uint32_t n   = m->src_n;
    const uint32_t last = n - 1u;
    uint32_t x = m->x0;
    for (uint32_t s = 0; s < n; s++) {
        const uint16_t v = src[last - s];
        if (v) dst[x] = v;
        x += rep[s];
    }
}

// av_emit_row() with the raster reversed -- the within-scanline half of a
// 180-degree turn. Equivalent to
//
//     for (x = m->x0; x < m->x1; x++) dst[x] = src[m->src_n - 1 - m->col[x]];
//
// NOTE `rep` is walked FORWARDS while `src` is walked backwards. Walking
// both backwards is the obvious guess and is wrong unless rep[] happens to
// be a palindrome, which it is not: canvas column x still takes its width
// from rep[col[x]], and only the pixel VALUE mirrors. Caught by
// tools/geom_test.
static inline void av_emit_row_rev(uint16_t *dst, const uint16_t *src,
                                   const av_map_t *m) {
    const uint8_t *rep = m->rep;      // hoisted -- see av_emit_row()
    const uint32_t n   = m->src_n;
    const uint32_t x1  = m->x1;
    const uint32_t last = n - 1u;
    uint32_t x = m->x0;
    for (uint32_t s = 0; s < n; s++) {
        const uint16_t v = src[last - s];
        dst[x]     = v;
        dst[x + 1] = v;
        x += rep[s];
    }
    dst[x1]      = 0;
    dst[x1 + 1u] = 0;
}

// ---------------------------------------------------------------------------
// WIDE-STORE UPSAMPLING EMIT
//
// The scalar av_emit_row() above writes TWO 16-bit stores per source pixel.
// At 240 source samples that is 480 halfword stores a scanline, against the
// 120 word stores a 1:1 path needs -- and it is why aspect correction was
// unaffordable: measured on Burger Time it cost 1,712us a frame, putting
// mean work over the whole 16,667us frame budget (DEVNOTES #88).
//
// This writes TWO CANVAS PIXELS PER 32-BIT STORE, the same trick
// btime_video.cpp's 1:1 path has always used, which was believed impossible
// for an upsample because the run pattern is irregular. It is not
// irregular. Every upsample here is "every k-th source pixel doubled, first
// of its group" (see av_map_t::dbl_k), so a group of k source pixels is
// exactly k+1 canvas pixels, and:
//
//   k=3  3 src -> 4 dst  = 2 words   (240 -> 320, Burger Time)
//   k=9  9 src -> 10 dst = 5 words   (288 -> 320, the Namco games)
//   k=4  8 src -> 10 dst = 5 words   (256 -> 320, Invaders/LRescue/DKong)
//
// k=4 takes TWO groups because 4+1 is odd and a single group would leave
// the pointer half-word aligned; two groups restore it. Odd k needs only
// one group.
//
// The group bodies are macros so the pattern exists ONCE. Every emit below
// -- forward, reversed, direct, palette-indexed -- instantiates the same
// three macros with a different FETCH. That is deliberate: the reversed
// variants are where this project has twice shipped a slow or wrong twin
// (DEVNOTES #81, and av_emit_row_rev()'s own warning), and a macro cannot
// drift from its sibling.
//
// FETCH(i) must yield the i-th SOURCE sample as a uint32_t already in
// canvas pixel format.
#define AV__W_K3(FETCH, O, N)                                                 \
    do {                                                                      \
        const uint32_t g_ = (N) / 3u;                                         \
        for (uint32_t i_ = 0; i_ < g_; i_++) {                                \
            const uint32_t s_ = i_ * 3u;                                      \
            const uint32_t a0 = FETCH(s_ + 0u);                               \
            const uint32_t a1 = FETCH(s_ + 1u);                               \
            const uint32_t a2 = FETCH(s_ + 2u);                               \
            (O)[i_ * 2u + 0u] = a0 | (a0 << 16);                              \
            (O)[i_ * 2u + 1u] = a1 | (a2 << 16);                              \
        }                                                                     \
    } while (0)

#define AV__W_K9(FETCH, O, N)                                                 \
    do {                                                                      \
        const uint32_t g_ = (N) / 9u;                                         \
        for (uint32_t i_ = 0; i_ < g_; i_++) {                                \
            const uint32_t s_ = i_ * 9u;                                      \
            const uint32_t a0 = FETCH(s_ + 0u), a1 = FETCH(s_ + 1u);          \
            const uint32_t a2 = FETCH(s_ + 2u), a3 = FETCH(s_ + 3u);          \
            const uint32_t a4 = FETCH(s_ + 4u), a5 = FETCH(s_ + 5u);          \
            const uint32_t a6 = FETCH(s_ + 6u), a7 = FETCH(s_ + 7u);          \
            const uint32_t a8 = FETCH(s_ + 8u);                               \
            (O)[i_ * 5u + 0u] = a0 | (a0 << 16);                              \
            (O)[i_ * 5u + 1u] = a1 | (a2 << 16);                              \
            (O)[i_ * 5u + 2u] = a3 | (a4 << 16);                              \
            (O)[i_ * 5u + 3u] = a5 | (a6 << 16);                              \
            (O)[i_ * 5u + 4u] = a7 | (a8 << 16);                              \
        }                                                                     \
    } while (0)

// Two k=4 groups at once: 8 src -> 10 dst. a0 and a4 are the doubled ones.
#define AV__W_K4(FETCH, O, N)                                                 \
    do {                                                                      \
        const uint32_t g_ = (N) / 8u;                                         \
        for (uint32_t i_ = 0; i_ < g_; i_++) {                                \
            const uint32_t s_ = i_ * 8u;                                      \
            const uint32_t a0 = FETCH(s_ + 0u), a1 = FETCH(s_ + 1u);          \
            const uint32_t a2 = FETCH(s_ + 2u), a3 = FETCH(s_ + 3u);          \
            const uint32_t a4 = FETCH(s_ + 4u), a5 = FETCH(s_ + 5u);          \
            const uint32_t a6 = FETCH(s_ + 6u), a7 = FETCH(s_ + 7u);          \
            (O)[i_ * 5u + 0u] = a0 | (a0 << 16);                              \
            (O)[i_ * 5u + 1u] = a1 | (a2 << 16);                              \
            (O)[i_ * 5u + 2u] = a3 | (a4 << 16);                              \
            (O)[i_ * 5u + 3u] = a4 | (a5 << 16);                              \
            (O)[i_ * 5u + 4u] = a6 | (a7 << 16);                              \
        }                                                                     \
    } while (0)

// Dispatch. `dbl_k` is 0 unless build_dbl_k() verified the shape AND x0 is
// even, so the only remaining risk is a scanline buffer that is not itself
// 4-byte aligned -- checked once per row, not per pixel, and falling back
// rather than making an unaligned word store.
#define AV__W_DISPATCH(FETCH, DST, M, FALLBACK)                               \
    do {                                                                      \
        const uint16_t k_ = (M)->dbl_k;                                       \
        uint32_t *o_ = (uint32_t *)(void *)((DST) + (M)->x0);                 \
        if (k_ == 0u || (((uintptr_t)o_) & 3u) != 0u) { FALLBACK; return; }   \
        const uint32_t n_ = (M)->src_n;                                       \
        if (k_ == 3u)      AV__W_K3(FETCH, o_, n_);                           \
        else if (k_ == 4u) AV__W_K4(FETCH, o_, n_);                           \
        else               AV__W_K9(FETCH, o_, n_);                           \
    } while (0)

// Scalar palette-indexed emits: av_emit_row()/_rev() for renderers whose
// row is 8-bit pen indices rather than canvas pixels. These are the
// fallbacks for the wide versions below, and exist so a paletted renderer
// never has to make an extra uint16 pass just to call the shared emit --
// which would cost more than the emit saves (DEVNOTES #79).
static inline void av_emit_row_pal(uint16_t *dst, const uint8_t *pen,
                                   const uint16_t *pal, const av_map_t *m) {
    const uint8_t *rep = m->rep;
    const uint32_t n   = m->src_n;
    const uint32_t x1  = m->x1;
    uint32_t x = m->x0;
    for (uint32_t s = 0; s < n; s++) {
        const uint16_t v = pal[pen[s]];
        dst[x] = v; dst[x + 1] = v;
        x += rep[s];
    }
    dst[x1] = 0; dst[x1 + 1u] = 0;
}

static inline void av_emit_row_pal_rev(uint16_t *dst, const uint8_t *pen,
                                       const uint16_t *pal, const av_map_t *m) {
    const uint8_t *rep = m->rep;      // FORWARDS -- only values mirror
    const uint32_t n   = m->src_n;
    const uint32_t x1  = m->x1;
    const uint32_t last = n - 1u;
    uint32_t x = m->x0;
    for (uint32_t s = 0; s < n; s++) {
        const uint16_t v = pal[pen[last - s]];
        dst[x] = v; dst[x + 1] = v;
        x += rep[s];
    }
    dst[x1] = 0; dst[x1 + 1u] = 0;
}

// The four wide emits. Each writes exactly the picture's own columns
// [x0, x1) and nothing else -- unlike the scalar versions there is NO
// overspill, so a caller that clears its borders separately is done.
static inline void av_emit_row_wide(uint16_t *dst, const uint16_t *src,
                                    const av_map_t *m) {
#define AV__F(i) ((uint32_t)src[(i)])
    AV__W_DISPATCH(AV__F, dst, m, av_emit_row(dst, src, m));
#undef AV__F
}

static inline void av_emit_row_wide_rev(uint16_t *dst, const uint16_t *src,
                                        const av_map_t *m) {
    const uint32_t last_ = (uint32_t)m->src_n - 1u;
#define AV__F(i) ((uint32_t)src[last_ - (i)])
    AV__W_DISPATCH(AV__F, dst, m, av_emit_row_rev(dst, src, m));
#undef AV__F
}

static inline void av_emit_row_wide_pal(uint16_t *dst, const uint8_t *pen,
                                        const uint16_t *pal, const av_map_t *m) {
#define AV__F(i) ((uint32_t)pal[pen[(i)]])
    AV__W_DISPATCH(AV__F, dst, m, av_emit_row_pal(dst, pen, pal, m));
#undef AV__F
}

static inline void av_emit_row_wide_pal_rev(uint16_t *dst, const uint8_t *pen,
                                            const uint16_t *pal, const av_map_t *m) {
    const uint32_t last_ = (uint32_t)m->src_n - 1u;
#define AV__F(i) ((uint32_t)pal[pen[last_ - (i)]])
    AV__W_DISPATCH(AV__F, dst, m, av_emit_row_pal_rev(dst, pen, pal, m));
#undef AV__F
}

#ifdef __cplusplus
}
#endif

#endif
