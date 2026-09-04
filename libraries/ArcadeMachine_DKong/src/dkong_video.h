// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Donkey Kong tile+sprite renderer -- board-agnostic (drives ArcadeHAL's
// video contract, never a specific board's display library).
//
// Same shape as ArcadeMachine_Pacman's renderer -- a tilemap plus hardware
// sprites, decoded once into pen caches at load time and rendered one
// physical scanline at a time on demand -- but three things differ, all
// verified against MAME's dkong_v.cpp:
//
//   - The tilemap is 32x32 of 8x8 tiles at 2bpp, and its per-tile COLOUR
//     does not come from a colour RAM. It comes from a PROM (v-5e.bpr),
//     indexed by a formula that folds four tile rows onto one PROM row:
//     `color_codes[tile_index % 32 + 32 * (tile_index / 32 / 4)]`.
//   - Sprites are selected PER SCANLINE, with a hard limit of 16, exactly
//     as the hardware's 64x9 line buffer imposes. MAME reproduces this by
//     calling screen_update via update_partial() once per scanline. That
//     suits this project's renderer perfectly, which is already per-scanline.
//   - The palette is not a direct PROM lookup. It is the output of a
//     resistor network fed by two PROMs, computed once at load time -- see
//     dkong_video.cpp's palette section.
#ifndef DKONG_VIDEO_H
#define DKONG_VIDEO_H

#include "dkong_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

// Raw ROM/PROM staging buffers, filled by dkong_assets.cpp and consumed
// once by dkong_video_build_caches(). Same pattern as pacman_video.h's.
#define DKONG_GFX1_SIZE        0x1000 // 2 tile ROMs   (v_5h_b.bin, v_3pt.bin)
#define DKONG_GFX2_SIZE        0x2000 // 4 sprite ROMs (l_4m/4n/4r/4s_b.bin)
#define DKONG_PALETTE_PROM_SIZE 0x0200 // c-2k.bpr then c-2j.bpr, 256 bytes each
#define DKONG_COLOR_PROM_SIZE   0x0100 // v-5e.bpr, per-column character colour codes

extern uint8_t dkong_gfx1[DKONG_GFX1_SIZE];
extern uint8_t dkong_gfx2[DKONG_GFX2_SIZE];
extern uint8_t dkong_palette_prom[DKONG_PALETTE_PROM_SIZE];
extern uint8_t dkong_color_prom[DKONG_COLOR_PROM_SIZE];

// Decodes the staging buffers above into the pen/palette caches the
// renderer uses. Call once, after dkong_load_rom() and before the first
// frame; the staging buffers may be reused or ignored afterwards.
void dkong_video_build_caches(void);

// Renders and submits one full frame from `system`'s VRAM/sprite RAM,
// honouring rotation, mirror_x and flip_screen. Retained for callers that
// want a standalone frame with no CPU to interleave; dkong_run_frame()
// drives the pump itself instead (see DEVNOTES.md problems #20/#34/#36).
void dkong_draw_frame(dkong_system *system);

// Renders ONE physical scanline (dvi_y) into `buf`, without acquiring or
// submitting it.
// Latches this frame's sprites for the column renderer that landscape uses.
// MUST be called once per frame before the first landscape scanline; tate
// does not need it (render_native_row() arbitrates per scanline itself).
// See dkong_video.cpp's COLUMN RENDERING comment and DEVNOTES #92.
void dkong_video_begin_frame(const dkong_system *system);

void dkong_video_render_scanline(const dkong_system *system, uint32_t dvi_y, uint16_t *buf);

// Per-frame render cost split, reset on read. `rows_us`/`rows` cover
// render_native_row(); `emit_us` covers turning that row into canvas pixels;
// `lines` is how many scanlines did any work. `rows` < `lines` means the
// duplicate-row memoisation is firing (see arcade_video_geom.h). Device-only
// numbers -- the host harness returns zeros for the timings.
void dkong_debug_take_render(uint32_t *rows_us, uint32_t *emit_us,
                             uint32_t *rows, uint32_t *lines);

// Boot-time asset-load error screen: floods every scanline with a solid
// colour, bypassing game VRAM entirely.
void dkong_draw_error_frame(uint16_t color);

// DEBUG: peak number of sprites selected on any one scanline since the last
// call, and how many times the hardware's 16-per-scanline limit was hit,
// then resets both. See tools/dkong_host/main.cpp.
void dkong_video_debug_take_sprite_stats(uint32_t *out_peak, uint32_t *out_limit_hits);

#ifdef __cplusplus
}
#endif

#endif
