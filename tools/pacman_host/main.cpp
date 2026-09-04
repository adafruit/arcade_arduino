// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Host-harness driver for ArcadeMachine_Pacman -- see ../README.md.
//
// Same idea as galaga_host: runs the REAL Pac-Man machine (the actual Z80
// core, real ROMs/PROMs, real port decode and per-scanline frame
// interleaving) natively against the shared stub ArcadeHAL in
// ../host_common/, so behaviour can be tested in ~a second instead of ~3
// minutes per hardware flash.
//
// The reason this one exists at all is `--seed-cyc`. pacman_machine.cpp's
// frame loop is built around surviving the wraparound of the Z80's 32-bit
// cycle counter, which at 3.072MHz happens roughly every 23 MINUTES of
// runtime -- DEVNOTES.md problem #22 records a real permanent hang from
// exactly that. Waiting out 23 minutes on hardware to test it is
// impractical, so nobody did, and the equivalent bug in Galaga went
// undetected for a long time. Seeding the counter close to the boundary
// turns that into a couple of seconds.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#include "pacman_machine.h"
#include "pacman_video.h"
#include "pacman_assets.h"
#include "pacman_input.h"
#include "arcade_hal_video.h"
#include "arcade_video_geom.h"
#include "host_ppm.h"
#include "z80.h"

extern "C" void host_storage_set_rom_dir(const char *dir);

pacman_system g_system;

static long g_frame = 0;

// Renders a full frame through the REAL renderer and writes it as a PPM,
// so screen content can be inspected with no hardware and no camera. The
// sampling and pixel-doubling live in host_ppm.cpp -- read its header
// before trusting a dump, because the private copy this replaced rendered
// landscape at a sample rate the device does not use.
static void ppm_render(void *ctx, uint32_t dvi_y, uint16_t *buf) {
    pacman_video_render_scanline((const pacman_system *)ctx, dvi_y, buf);
}

static void dump_ppm(const char *path) {
    if (host_ppm_write(path, ppm_render, &g_system))
        printf("[wrote %s at frame %ld]\n", path, g_frame);
}

static void print_state(const char *tag) {
    printf("--- %s (frame %ld) ---\n", tag, g_frame);
    printf("  cpu pc=%04X sp=%04X cyc=%" PRIu32 "\n",
           g_system.cpu.pc, g_system.cpu.sp, g_system.cpu.cyc);
    printf("  rotation=%u mirror=%d\n",
           (unsigned)g_system.rotation, (int)g_system.mirror_x);
}

// Scripted input, same shape as galaga_host's.
struct InputEvent { long frame; int btn; };
static InputEvent g_events[64];
static int        g_event_n = 0;
static long       g_press_frames = 6;
static const char *BTNNAME[7] = {"coin","start1","start2","up","down","left","right"};

static bool btn_active(int btn, long frame) {
    for (int i = 0; i < g_event_n; i++)
        if (g_events[i].btn == btn &&
            frame >= g_events[i].frame && frame < g_events[i].frame + g_press_frames)
            return true;
    return false;
}

static void parse_events(char *spec) {
    for (char *tok = strtok(spec, ","); tok && g_event_n < 64; tok = strtok(NULL, ",")) {
        char *colon = strchr(tok, ':');
        if (!colon) continue;
        *colon = 0;
        long fr = atol(tok);
        const char *name = colon + 1;
        for (int b = 0; b < 7; b++)
            if (!strcmp(name, BTNNAME[b])) { g_events[g_event_n].frame = fr;
                                             g_events[g_event_n].btn = b; g_event_n++; break; }
    }
}

static const char *find_rom_dir(const char *explicit_dir) {
    static const char *cands[] = {
        NULL, NULL,
        "pacman_assets/rom", "../pacman_assets/rom", "../../pacman_assets/rom",
        "../../../pacman_assets/rom", "../../../../pacman_assets/rom",
    };
    cands[0] = explicit_dir;
    cands[1] = getenv("PACMAN_ROM_DIR");
    for (unsigned i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) {
        if (!cands[i]) continue;
        char probe[2048];
        snprintf(probe, sizeof(probe), "%s/pacman.6e", cands[i]);
        FILE *fp = fopen(probe, "rb");
        if (fp) { fclose(fp); return cands[i]; }
    }
    return NULL;
}

static void usage(const char *argv0) {
    printf("usage: %s [options]\n"
           "  --rom DIR       ROM directory (default: search for pacman_assets/rom,\n"
           "                  or $PACMAN_ROM_DIR)\n"
           "  --rotation N    override the machine's default screen rotation\n"
           "  --stretch       aspect-ratio correction: fill 320x240 in tate,\n"
           "                  180x240 in yoko, instead of 1:1 with pillarbox\n"
           "                  (0=landscape 1=90 CCW 2=180 3=90 CW), so an\n"
           "                  orientation can be checked without hardware\n"
           "  --frames N      frames to run (default 3000)\n"
           "  --seed-cyc N    set the Z80 cycle counter to N right after init, before\n"
           "                  any cycles run. Use a value just under 2^32 (4294967296)\n"
           "                  to reach the ~23-minute wraparound in seconds -- e.g.\n"
           "                  --seed-cyc 4294800000 wraps after ~3 frames. See\n"
           "                  DEVNOTES.md problem #22.\n"
           "  --every N       print a state block every N frames (default 0 = off)\n"
           "  --ppm-every N   dump a rendered PPM every N frames\n"
           "  --ppm-prefix P  filename prefix for PPM dumps (default \"frame\")\n"
           "  --stall N       exit 3 if the rendered frame is byte-identical for N\n"
           "                  consecutive checks (default 0 = off; needs --ppm-every)\n"
           "  --input SPEC    scripted presses, e.g. 600:coin,800:start1,900:left\n"
           "                  (buttons: coin start1 start2 up down left right)\n"
           "  --press-frames N  how long each scripted press is held (default 6)\n",
           argv0);
}

int main(int argc, char **argv) {
    const char *rom_arg = NULL, *ppm_prefix = "frame";
    long frames = 3000, every = 0, ppm_every = 0, stall_lim = 0;
    long rotation = -1;
    bool stretch = false;
    unsigned long long seed_cyc = 0;
    bool do_seed = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc)             rom_arg = argv[++i];
        else if (!strcmp(argv[i], "--rotation") && i + 1 < argc)   rotation = atol(argv[++i]);
        else if (!strcmp(argv[i], "--stretch"))                    stretch = true;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)     frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--every") && i + 1 < argc)      every = atol(argv[++i]);
        else if (!strcmp(argv[i], "--ppm-every") && i + 1 < argc)  ppm_every = atol(argv[++i]);
        else if (!strcmp(argv[i], "--ppm-prefix") && i + 1 < argc) ppm_prefix = argv[++i];
        else if (!strcmp(argv[i], "--stall") && i + 1 < argc)      stall_lim = atol(argv[++i]);
        else if (!strcmp(argv[i], "--press-frames") && i + 1 < argc) g_press_frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--input") && i + 1 < argc)      parse_events(argv[++i]);
        else if (!strcmp(argv[i], "--seed-cyc") && i + 1 < argc)   { seed_cyc = strtoull(argv[++i], NULL, 0); do_seed = true; }
        else { usage(argv[0]); return 2; }
    }

    const char *rom_dir = find_rom_dir(rom_arg);
    if (!rom_dir) {
        fprintf(stderr, "error: could not locate a Pac-Man ROM directory "
                        "(looked for pacman.6e). Use --rom DIR.\n");
        return 1;
    }
    printf("rom dir: %s\n", rom_dir);
    host_storage_set_rom_dir(rom_dir);

    pacman_init(&g_system);
    // Applied after init, which is where each machine sets its own default
    // (see that machine's *_init). Lets an orientation be checked in the
    // harness rather than by flashing and physically turning a monitor.
    if (rotation >= 0 && rotation <= 3) g_system.rotation = (uint8_t)rotation;
    // Aspect-ratio correction (arcade_video_geom.h). Applied after
    // pacman_init(), which is where av_geom_init() built the maps.
    if (stretch) av_geom_set_stretch(true);

    uint16_t err = 0;
    if (!pacman_load_assets(&g_system, &err)) {
        fprintf(stderr, "error: pacman_load_assets failed (error color %04X)\n", err);
        return 1;
    }

    // Seed AFTER init/asset load but BEFORE any cycles run, so anything the
    // machine derives from the counter is computed relative to this base.
    if (do_seed) {
        g_system.cpu.cyc = (uint32_t)seed_cyc;
        printf("seeded cpu.cyc = %" PRIu32 " (wraps in ~%.1f frames)\n",
               g_system.cpu.cyc,
               (double)(4294967296.0 - (double)g_system.cpu.cyc) / 50688.0);
    }

    printf("running %ld frames...\n\n", frames);

    static uint16_t prev_row[4096], cur_row[4096];
    long identical_streak = 0;

    for (g_frame = 0; g_frame < frames; g_frame++) {
        pacman_input_update(&g_system,
                            btn_active(0, g_frame),  // coin
                            btn_active(1, g_frame),  // start1
                            btn_active(2, g_frame),  // start2
                            btn_active(3, g_frame),  // up
                            btn_active(4, g_frame),  // down
                            btn_active(5, g_frame),  // left
                            btn_active(6, g_frame),  // right
                            false, false);           // rotate/mirror meta
        pacman_run_frame(&g_system);

        if (every > 0 && (g_frame % every) == 0) print_state("state");

        if (ppm_every > 0 && (g_frame % ppm_every) == 0) {
            char path[1024];
            snprintf(path, sizeof(path), "%s_%05ld.ppm", ppm_prefix, g_frame);
            dump_ppm(path);

            if (stall_lim > 0) {
                // Cheap liveness probe: one mid-screen scanline. If the
                // picture stops changing entirely, the machine has hung.
                memcpy(prev_row, cur_row, sizeof(cur_row));
                memset(cur_row, 0, sizeof(cur_row));
                pacman_video_render_scanline(&g_system, HAL_VIDEO_HEIGHT / 2, cur_row);
                if (g_frame > 0 && memcmp(prev_row, cur_row, sizeof(cur_row)) == 0) {
                    if (++identical_streak >= stall_lim) {
                        printf("\n*** STALLED: rendered scanline unchanged for %ld checks ***\n",
                               identical_streak);
                        print_state("stall");
                        return 3;
                    }
                } else identical_streak = 0;
            }
        }
    }

    print_state("final");
    return 0;
}
