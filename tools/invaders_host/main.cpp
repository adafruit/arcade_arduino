// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Host-harness driver for ArcadeMachine_Invaders -- see ../README.md.
//
// Same idea as pacman_host/galaga_host: runs the REAL Space Invaders machine
// (the actual i8080 core, real ROM chips, real port decode, real WAV sample
// loading, real per-scanline frame interleaving) natively against the shared
// stub ArcadeHAL in ../host_common/.
//
// The reason THIS one exists is `--digest`. DEVNOTES.md problem #34's fix --
// splitting a machine's frame loop so CPU execution interleaves with
// scanline submission -- is a change to WHEN emulated instructions run
// relative to video output, and it must not change WHICH instructions run or
// when the two per-frame interrupts fire. That claim is checkable rather
// than arguable: run N frames before and after the change and compare a
// digest of the whole machine state. Rendered output legitimately differs
// (that is the point of interleaving -- a scanline now reflects mid-frame
// VRAM, as real CRT hardware does), so a PPM comparison is the WRONG
// instrument here and would report a false failure. See DEVNOTES.md problem
// #32 for how much a proxy measurement can cost on this project.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#include "invaders_machine.h"
#include "invaders_video.h"
#include "invaders_assets.h"
#include "invaders_input.h"
#include "arcade_hal_video.h"
#include "arcade_video_geom.h"
#include "host_ppm.h"

extern "C" void host_storage_set_rom_dir(const char *dir);
extern "C" void host_storage_set_samples_dir(const char *dir);

static arcade_system g_system;
static long          g_frame = 0;

// FNV-1a over the entire emulated machine: CPU registers, SP, PC, condition
// codes, interrupt-enable, the full 64K address space (which includes VRAM
// at 0x2400), and the external shift register. Two builds that agree on this
// after thousands of frames have executed the identical instruction stream
// with the identical interrupt timing.
static uint64_t digest_state(void) {
    uint64_t h = 1469598103934665603ULL;
    const uint8_t *p;
    size_t n;

    #define FNV(ptr, len) do { p = (const uint8_t *)(ptr); n = (len); \
        for (size_t _i = 0; _i < n; _i++) { h ^= p[_i]; h *= 1099511628211ULL; } } while (0)

    FNV(g_system.state.regs, sizeof(g_system.state.regs));
    FNV(&g_system.state.sp, sizeof(g_system.state.sp));
    FNV(&g_system.state.pc, sizeof(g_system.state.pc));
    FNV(&g_system.state.cc, sizeof(g_system.state.cc));
    FNV(&g_system.state.int_enable, sizeof(g_system.state.int_enable));
    FNV(g_system.state.memory, sizeof(g_system.state.memory));
    FNV(&g_system.ext_shift_data, sizeof(g_system.ext_shift_data));
    FNV(&g_system.ext_shift_offset, sizeof(g_system.ext_shift_offset));
    #undef FNV

    return h;
}

// Renders a full frame through the REAL renderer and writes it as a PPM, so
// screen content can be inspected with no hardware and no camera.
// Sampling and pixel-doubling live in host_ppm.cpp -- read its header
// before trusting a dump. Note this renderer takes its system pointer
// LAST, unlike its siblings.
static void ppm_render(void *ctx, uint32_t dvi_y, uint16_t *buf) {
    invaders_video_render_scanline(dvi_y, buf, (const arcade_system *)ctx);
}

static void dump_ppm(const char *path) {
    if (host_ppm_write(path, ppm_render, &g_system))
        printf("[wrote %s at frame %ld]\n", path, g_frame);
}

static void print_state(const char *tag) {
    printf("--- %s (frame %ld) ---\n", tag, g_frame);
    printf("  cpu pc=%04X sp=%04X a=%02X int_enable=%u\n",
           g_system.state.pc, g_system.state.sp,
           g_system.state.regs[6], (unsigned)g_system.state.int_enable);
    printf("  rotation=%u mirror=%d shift=%04X/%u\n",
           (unsigned)g_system.rotation, (int)g_system.mirror_x,
           g_system.ext_shift_data, (unsigned)g_system.ext_shift_offset);
    printf("  digest=%016" PRIX64 "\n", digest_state());
}

// Scripted input, same shape as pacman_host's.
struct InputEvent { long frame; int btn; };
static InputEvent g_events[64];
static int        g_event_n = 0;
static long       g_press_frames = 6;
static const char *BTNNAME[6] = {"coin","start1","start2","left","right","shoot"};

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
        for (int b = 0; b < 6; b++)
            if (!strcmp(name, BTNNAME[b])) { g_events[g_event_n].frame = fr;
                                             g_events[g_event_n].btn = b; g_event_n++; break; }
    }
}

// `out` is caller-owned on purpose: an earlier version returned a pointer
// into one shared static buffer, so the second call silently rewrote the
// first call's answer and the ROM directory ended up aliased to the samples
// directory -- which did NOT fail loudly, because loading WAV files as ROM
// chips still reads bytes and still reports success.
static bool find_asset_dir(const char *explicit_dir, const char *sub,
                           const char *probe_file, char *out, size_t out_len) {
    static const char *cands[] = {
        NULL, NULL,
        "invaders_assets", "../invaders_assets", "../../invaders_assets",
        "../../../invaders_assets", "../../../../invaders_assets",
    };
    cands[0] = explicit_dir;
    cands[1] = getenv("INVADERS_ASSET_DIR");
    for (unsigned i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) {
        if (!cands[i]) continue;
        char probe[2048];
        snprintf(probe, sizeof(probe), "%s/%s/%s", cands[i], sub, probe_file);
        FILE *fp = fopen(probe, "rb");
        if (!fp) continue;
        fclose(fp);
        snprintf(out, out_len, "%s/%s", cands[i], sub);
        return true;
    }
    return false;
}

static void usage(const char *argv0) {
    printf("usage: %s [options]\n"
           "  --assets DIR    directory containing rom/ and samples/ (default:\n"
           "                  search upward for invaders_assets, or\n"
           "                  $INVADERS_ASSET_DIR)\n"
           "  --rotation N    override the machine's default screen rotation\n"
           "                  (0=landscape 1=90 CCW 2=180 3=90 CW), so an\n"
           "                  orientation can be checked without hardware\n"
           "  --frames N      frames to run (default 3000)\n"
           "  --every N       print a state block every N frames (default 0 = off)\n"
           "  --digest-every N  print just frame+digest every N frames -- the A/B\n"
           "                  instrument for changes that must not alter emulation\n"
           "                  (see this file's header comment)\n"
           "  --ppm-every N   dump a rendered PPM every N frames\n"
           "  --ppm-prefix P  filename prefix for PPM dumps (default \"frame\")\n"
           "  --input SPEC    scripted presses, e.g. 60:coin,120:start1,200:shoot\n"
           "                  (buttons: coin start1 start2 left right shoot)\n"
           "  --press-frames N  how long each scripted press is held (default 6)\n",
           argv0);
}

int main(int argc, char **argv) {
    const char *asset_arg = NULL, *ppm_prefix = "frame";
    long frames = 3000, every = 0, digest_every = 0, ppm_every = 0;
    long rotation = -1;
    bool stretch = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--assets") && i + 1 < argc)             asset_arg = argv[++i];
        else if (!strcmp(argv[i], "--rotation") && i + 1 < argc)      rotation = atol(argv[++i]);
        else if (!strcmp(argv[i], "--stretch"))                   stretch = true;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)        frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--every") && i + 1 < argc)         every = atol(argv[++i]);
        else if (!strcmp(argv[i], "--digest-every") && i + 1 < argc)  digest_every = atol(argv[++i]);
        else if (!strcmp(argv[i], "--ppm-every") && i + 1 < argc)     ppm_every = atol(argv[++i]);
        else if (!strcmp(argv[i], "--ppm-prefix") && i + 1 < argc)    ppm_prefix = argv[++i];
        else if (!strcmp(argv[i], "--press-frames") && i + 1 < argc)  g_press_frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--input") && i + 1 < argc)         parse_events(argv[++i]);
        else { usage(argv[0]); return 2; }
    }

    static char rom_dir[2048], samples_dir[2048];
    if (!find_asset_dir(asset_arg, "rom", "invaders.h", rom_dir, sizeof(rom_dir))) {
        fprintf(stderr, "error: could not locate a Space Invaders ROM directory "
                        "(looked for invaders_assets/rom/invaders.h). Use --assets DIR.\n");
        return 1;
    }
    // Samples are optional to FIND but not to LOAD: invaders_load_assets()
    // fails outright if zero samples load (that is the yellow boot-error
    // screen on hardware), so the harness reports the directory it settled
    // on rather than failing later with a bare error color.
    bool have_samples = find_asset_dir(asset_arg, "samples", "0.wav",
                                       samples_dir, sizeof(samples_dir));
    printf("rom dir:     %s\n", rom_dir);
    printf("samples dir: %s\n", have_samples ? samples_dir : "(none found)");
    host_storage_set_rom_dir(rom_dir);
    if (have_samples) host_storage_set_samples_dir(samples_dir);

    invaders_init(&g_system);
    // Applied after init, which is where the machine sets its own default
    // (invaders_init: rotation 1, tate). Lets an orientation be checked in
    // the harness rather than by flashing and physically turning a monitor.
    if (rotation >= 0 && rotation <= 3) g_system.rotation = (uint8_t)rotation;
    // Aspect-ratio correction (arcade_video_geom.h), applied after the
    // machine init that built the maps.
    if (stretch) av_geom_set_stretch(true);

    uint16_t err = 0;
    if (!invaders_load_assets(&g_system, &err)) {
        fprintf(stderr, "error: invaders_load_assets failed (error color %04X: %s)\n",
                err, err == INVADERS_COLOR_ERROR_NO_CARD ? "no storage"
                                                         : "no ROM and/or sample files");
        return 1;
    }

    printf("running %ld frames...\n\n", frames);

    for (g_frame = 0; g_frame < frames; g_frame++) {
        invaders_input_update(&g_system,
                              btn_active(0, g_frame),  // coin
                              btn_active(1, g_frame),  // start1
                              btn_active(2, g_frame),  // start2
                              btn_active(3, g_frame),  // left
                              btn_active(4, g_frame),  // right
                              btn_active(5, g_frame),  // shoot
                              false, false);           // rotate/mirror meta
        invaders_run_frame(&g_system);

        if (every > 0 && (g_frame % every) == 0) print_state("state");
        if (digest_every > 0 && (g_frame % digest_every) == 0)
            printf("frame %6ld digest=%016" PRIX64 "\n", g_frame, digest_state());

        if (ppm_every > 0 && (g_frame % ppm_every) == 0) {
            char path[1024];
            snprintf(path, sizeof(path), "%s_%05ld.ppm", ppm_prefix, g_frame);
            dump_ppm(path);
        }
    }

    print_state("final");
    return 0;
}
