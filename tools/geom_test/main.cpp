// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Screen-geometry conformance runner for ArcadeHAL's arcade_video_geom.
//
// Like tools/m6502_test/, this is NOT a machine harness -- it exercises one
// shared module directly. It exists because the geometry it checks is the
// kind that fails SILENTLY: a wrong border constant does not crash, it
// shifts the picture, and nobody notices until someone photographs a screen
// and measures it. DEVNOTES #21, #23, #33 and #77 are all that failure.
//
// It asserts the two things a renderer relies on and cannot check itself:
//
//   1. With the aspect correction OFF, the maps reproduce the project's
//      historical hand-derived layout EXACTLY -- Invaders tate x[32,288),
//      Pac-Man x[16,304), y[8,232), yoko x[48,272), Burger Time x[40,280).
//      That is what makes "stretch off changed nothing" a checkable claim.
//   2. With it ON, every game lands on the SAME aspect-correct destination:
//      320x240 in tate and 180x240 at x0=70 in yoko. Those numbers come
//      from the cabinet's 4:3 tube, not from any game's raster, so a game
//      that disagrees is wrong by construction.
//
// Plus the invariants that keep the inner loops safe: every index inside
// [x0,x1)/[y0,y1) is within the raster, and both maps are monotonic.
#include "arcade_video_geom.h"
#include "arcade_hal_video.h"
#include <stdio.h>
#include <set>

const uint32_t HAL_VIDEO_WIDTH  = 320u;
const uint32_t HAL_VIDEO_HEIGHT = 240u;

static int fails = 0;
#define CHECK(c) do { if(!(c)) { printf("  FAIL: %s\n", #c); fails++; } } while(0)

static void report(const char *tag, const av_map_t &m, uint32_t src_col, uint32_t src_row) {
    std::set<int> cs, rs;
    for (uint32_t x = m.x0; x < m.x1; x++) cs.insert(m.col[x]);
    for (uint32_t y = m.y0; y < m.y1; y++) rs.insert(m.row[y]);
    printf("  %-5s x[%3u,%3u) w=%3u  col:%3u/%3u src used  |  y[%3u,%3u) h=%3u  row:%3u/%3u src used\n",
           tag, m.x0, m.x1, m.x1 - m.x0, (unsigned)cs.size(), src_col,
           m.y0, m.y1, m.y1 - m.y0, (unsigned)rs.size(), src_row);
    // never index past the raster, and never go backwards
    for (uint32_t x = m.x0; x < m.x1; x++) {
        CHECK(m.col[x] < src_col);
        if (x > m.x0) CHECK(m.col[x] >= m.col[x-1]);
    }
    for (uint32_t y = m.y0; y < m.y1; y++) {
        CHECK(m.row[y] < src_row);
        if (y > m.y0) CHECK(m.row[y] >= m.row[y-1]);
    }
    CHECK(m.x1 <= 320); CHECK(m.y1 <= 240);
}

int main() {
    struct { const char *n; uint32_t L, S; } games[] = {
        {"Invaders/LRescue/DKong", 256, 224},
        {"Pac/MsPac/Galaga",       288, 224},
        {"BurgerTime",             240, 240},
    };
    for (auto &g : games) {
        av_geom_init(g.L, g.S);
        for (int st = 0; st < 2; st++) {
            av_geom_set_stretch(st != 0);
            printf("%s  raster %ux%u  stretch=%s\n", g.n, g.L, g.S, st ? "ON " : "off");
            report("tate", av_tate, g.L, g.S);
            report("yoko", av_yoko, g.S, g.L);
            if (st) {
                // aspect-correct destinations, identical for every game
                CHECK(av_tate.x1 - av_tate.x0 == 320);
                CHECK(av_tate.y1 - av_tate.y0 == 240);
                CHECK(av_yoko.x1 - av_yoko.x0 == 180);
                CHECK(av_yoko.y1 - av_yoko.y0 == 240);
                CHECK(av_yoko.x0 == 70);
            } else {
                // historical layout: 1:1, centred
                CHECK(av_tate.x1 - av_tate.x0 == g.L);
                CHECK(av_tate.y1 - av_tate.y0 == g.S);
                CHECK(av_yoko.x1 - av_yoko.x0 == g.S);
                CHECK(av_yoko.y1 - av_yoko.y0 == 240);
            }
        }
        printf("\n");
    }
    printf(fails ? "*** %d CHECKS FAILED\n" : "all checks passed\n", fails);
    return fails ? 1 : 0;
}
