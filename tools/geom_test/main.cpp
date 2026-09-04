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

// The whole point of rep[]: av_emit_row() must produce EXACTLY what the
// straightforward `dst[x] = src[col[x]]` produces, forwards and reversed.
// If it does not, turning the aspect correction on stops being a pure A/B
// and every byte-compare baseline becomes meaningless.
static void check_emit(const char *tag, const av_map_t &m) {
    static uint16_t src[512], got[1024], want[1024];
    for (uint32_t i = 0; i < 512; i++) src[i] = (uint16_t)(i * 7u + 1u); // distinct
    uint32_t sum = 0;
    for (uint32_t s = 0; s < m.src_n; s++) sum += m.rep[s];
    if (sum != (uint32_t)(m.x1 - m.x0)) {
        printf("  FAIL: %s sum(rep)=%u but width=%u\n", tag, sum, m.x1 - m.x0);
        fails++;
        return;
    }

    for (int rev = 0; rev < 2; rev++) {
        for (uint32_t i = 0; i < 1024; i++) { got[i] = 0; want[i] = 0; }
        for (uint32_t x = m.x0; x < m.x1; x++)
            want[x] = rev ? src[m.src_n - 1u - m.col[x]] : src[m.col[x]];
        if (rev) av_emit_row_rev(got, src, &m); else av_emit_row(got, src, &m);
        for (uint32_t x = m.x0; x < m.x1; x++) {
            if (got[x] != want[x]) {
                printf("  FAIL: %s%s x=%u got %u want %u\n",
                       tag, rev ? " (rev)" : "", x, got[x], want[x]);
                fails++;
                return;
            }
        }
        // and nothing painted outside the picture
        for (uint32_t x = 0; x < m.x0; x++) if (got[x]) { printf("  FAIL: %s left bleed\n", tag); fails++; return; }
        for (uint32_t x = m.x1; x < 1000; x++) if (got[x]) { printf("  FAIL: %s right bleed at %u\n", tag, x); fails++; return; }
    }
}

// The WIDE-STORE emits must produce byte-identical output to the scalar
// ones, in all four combinations (direct/paletted x forward/reversed).
//
// This matters more than a normal equivalence test. The wide emits write
// two pixels per 32-bit store from a hand-derived group pattern -- get one
// index wrong in AV__W_K4 and the picture is subtly sheared in ONE game, in
// ONE rotation, in a way that looks like a rendering bug rather than a
// resampling one. The reversed variants are worse: this project has twice
// shipped a reversed twin that was slower or wrong (DEVNOTES #81, and
// av_emit_row_rev()'s own palindrome warning). Proving them here costs
// nothing and needs no hardware.
static void check_emit_wide(const char *tag, const av_map_t &m) {
    static uint16_t src[512], got[1024], want[1024];
    static uint8_t  pen[512];
    static uint16_t pal[256];
    for (uint32_t i = 0; i < 512; i++) src[i] = (uint16_t)(i * 7u + 1u);
    // A paletted source that maps back to the SAME values, so one `want`
    // serves both variants: pen[i] = i & 0xFF, pal[p] chosen per group.
    for (uint32_t i = 0; i < 256; i++) pal[i] = (uint16_t)(i * 3u + 11u);
    for (uint32_t i = 0; i < 512; i++) pen[i] = (uint8_t)(i & 0xFFu);

    for (int rev = 0; rev < 2; rev++) {
        // --- direct ---
        for (uint32_t i = 0; i < 1024; i++) { got[i] = 0; want[i] = 0; }
        for (uint32_t x = m.x0; x < m.x1; x++)
            want[x] = rev ? src[m.src_n - 1u - m.col[x]] : src[m.col[x]];
        if (rev) av_emit_row_wide_rev(got, src, &m); else av_emit_row_wide(got, src, &m);
        for (uint32_t x = m.x0; x < m.x1; x++)
            if (got[x] != want[x]) {
                printf("  FAIL: %s wide%s x=%u got %u want %u (dbl_k=%u)\n",
                       tag, rev ? " (rev)" : "", x, got[x], want[x], m.dbl_k);
                fails++; return;
            }
        for (uint32_t x = 0; x < m.x0; x++) if (got[x]) { printf("  FAIL: %s wide left bleed\n", tag); fails++; return; }
        for (uint32_t x = m.x1; x < 1000; x++) if (got[x]) { printf("  FAIL: %s wide right bleed at %u\n", tag, x); fails++; return; }

        // --- palette-indexed, same expected picture ---
        for (uint32_t i = 0; i < 1024; i++) { got[i] = 0; want[i] = 0; }
        for (uint32_t x = m.x0; x < m.x1; x++) {
            const uint32_t si = rev ? (m.src_n - 1u - m.col[x]) : m.col[x];
            want[x] = pal[pen[si]];
        }
        if (rev) av_emit_row_wide_pal_rev(got, pen, pal, &m);
        else     av_emit_row_wide_pal(got, pen, pal, &m);
        for (uint32_t x = m.x0; x < m.x1; x++)
            if (got[x] != want[x]) {
                printf("  FAIL: %s wide_pal%s x=%u got %u want %u\n",
                       tag, rev ? " (rev)" : "", x, got[x], want[x]);
                fails++; return;
            }
    }
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
            check_emit("tate", av_tate);
            check_emit("yoko", av_yoko);
            check_emit_wide("tate", av_tate);
            check_emit_wide("yoko", av_yoko);
            printf("      dbl_k: tate=%u yoko=%u\n", av_tate.dbl_k, av_yoko.dbl_k);
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
