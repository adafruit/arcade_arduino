// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// See arcade_video_geom.h for the geometry this implements.
#include "arcade_video_geom.h"
#include "arcade_hal_video.h"

av_map_t av_tate;
av_map_t av_yoko;

static bool     s_stretch   = false;
static uint32_t s_long_px   = 0;
static uint32_t s_short_px  = 0;

// Nearest-neighbour resample of `src_px` raster samples onto canvas
// positions [dst0, dst0+dst_n), written into `t`.
//
// `(i * src_px) / dst_n` is exact for every ratio this project uses, so the
// spacing is uniform rather than ragged: 240->320 is x4/3, 256->320 is
// x5/4, 288->320 is x10/9, 224->240 is x15/14. Worth stating because the
// DOWN-scaling direction has the same property and it is easy to assume
// otherwise -- at 240 samples of a 288-entry axis the dropped lines land
// every 6th, exactly, not raggedly (DEVNOTES #76).
static bool build(uint16_t *t, uint16_t *d0, uint16_t *d1,
                  uint32_t dst0, uint32_t dst_n, uint32_t src_px) {
    for (uint32_t i = 0; i < dst_n; i++)
        t[dst0 + i] = (uint16_t)((i * src_px) / dst_n);
    *d0 = (uint16_t)dst0;
    *d1 = (uint16_t)(dst0 + dst_n);
    return src_px == dst_n; // identity shift iff no resampling happened
}

// The 1:1 identity case: `src_px` raster samples placed one-per-canvas-pixel
// and centred. This is the project's historical layout, and building it
// through the same table as the stretched case is what makes "stretch off"
// bit-for-bit unchanged rather than merely similar.
static bool build_1to1(uint16_t *t, uint16_t *d0, uint16_t *d1,
                       uint32_t canvas_n, uint32_t src_px) {
    if (src_px > canvas_n) src_px = canvas_n; // never overrun the canvas
    const uint32_t dst0 = (canvas_n - src_px) / 2u;
    for (uint32_t i = 0; i < src_px; i++)
        t[dst0 + i] = (uint16_t)i;
    *d0 = (uint16_t)dst0;
    *d1 = (uint16_t)(dst0 + src_px);
    return true;
}

// Derives rep[]/src_n from the col[] table that was just built, rather than
// computing it independently. Deliberate: two formulas for one mapping is
// exactly how a renderer and its border constant drift apart (DEVNOTES #77),
// and av_emit_row() and the `buf[x] = row[col[x]]` fallback have to agree
// pixel for pixel or the aspect toggle stops being a pure A/B.
static void build_rep(av_map_t *m, uint32_t src_px) {
    for (uint32_t s = 0; s < src_px && s < AV_CANVAS_W; s++) m->rep[s] = 0;
    for (uint32_t x = m->x0; x < m->x1; x++) m->rep[m->col[x]]++;
    m->src_n = (uint16_t)(src_px < AV_CANVAS_W ? src_px : AV_CANVAS_W);
}

// rowrep[y]: how many raster samples collapse into canvas row y. Derived
// from row[] for the same reason build_rep() is derived from col[] -- one
// mapping, one formula, no chance of the two disagreeing.
static void build_rowrep(av_map_t *m, uint32_t row_src_px) {
    for (uint32_t y = 0; y < AV_CANVAS_H; y++) m->rowrep[y] = 0;
    for (uint32_t y = m->y0; y < m->y1; y++) {
        const uint32_t next = (y + 1u < m->y1) ? m->row[y + 1u] : row_src_px;
        const uint32_t n    = next - m->row[y];
        m->rowrep[y] = (uint8_t)(n > 255u ? 255u : n);
    }
}

static void rebuild(void) {
    if (!s_long_px || !s_short_px) return;

    uint32_t cw = HAL_VIDEO_WIDTH  < AV_CANVAS_W ? HAL_VIDEO_WIDTH  : AV_CANVAS_W;
    uint32_t ch = HAL_VIDEO_HEIGHT < AV_CANVAS_H ? HAL_VIDEO_HEIGHT : AV_CANVAS_H;

    if (s_stretch) {
        // Fill the screen: LONG over every canvas column, SHORT over every
        // canvas row.
        av_tate.col_1to1 = build(av_tate.col, &av_tate.x0, &av_tate.x1, 0u, cw, s_long_px);
        build(av_tate.row, &av_tate.y0, &av_tate.y1, 0u, ch, s_short_px);

        // Yoko: LONG already fills all 240 rows; SHORT narrows to 180.
        const uint32_t w  = AV_YOKO_W < cw ? AV_YOKO_W : cw;
        av_yoko.col_1to1 = build(av_yoko.col, &av_yoko.x0, &av_yoko.x1, (cw - w) / 2u, w, s_short_px);
        build(av_yoko.row, &av_yoko.y0, &av_yoko.y1, 0u, ch, s_long_px);

        build_rep(&av_tate, s_long_px);
        build_rep(&av_yoko, s_short_px);
        build_rowrep(&av_tate, s_short_px);
        build_rowrep(&av_yoko, s_long_px);
    } else {
        // Historical layout: 1:1 with pillar/letterboxing, EXCEPT yoko's
        // row axis, which already resampled LONG onto all 240 canvas rows
        // and is therefore identical in both modes. That asymmetry is not
        // an oversight -- it is why landscape was 24.4% too wide while tate
        // was only 3.7-16.7%: one of yoko's two axes was already being
        // scaled and the other was not.
        av_tate.col_1to1 = build_1to1(av_tate.col, &av_tate.x0, &av_tate.x1, cw, s_long_px);
        build_1to1(av_tate.row, &av_tate.y0, &av_tate.y1, ch, s_short_px);

        av_yoko.col_1to1 = build_1to1(av_yoko.col, &av_yoko.x0, &av_yoko.x1, cw, s_short_px);
        build(av_yoko.row, &av_yoko.y0, &av_yoko.y1, 0u, ch, s_long_px);

        build_rep(&av_tate, s_long_px);
        build_rep(&av_yoko, s_short_px);
        build_rowrep(&av_tate, s_short_px);
        build_rowrep(&av_yoko, s_long_px);
    }
}

void av_geom_init(uint32_t long_px, uint32_t short_px) {
    s_long_px  = long_px;
    s_short_px = short_px;
    rebuild();
}

void av_geom_set_stretch(bool on) { s_stretch = on; rebuild(); }
bool av_geom_get_stretch(void)    { return s_stretch; }
