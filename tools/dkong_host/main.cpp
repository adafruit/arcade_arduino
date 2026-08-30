// Host-harness driver for ArcadeMachine_DKong -- see ../README.md.
//
// Runs the REAL Donkey Kong machine (the actual Z80 core, real ROMs/PROMs,
// the real i8257 DMA, real port decode and per-scanline frame interleaving)
// natively against the shared stub ArcadeHAL in ../host_common/.
//
// The flag that matters here is `--dma`. Donkey Kong's sprites reach the
// video hardware ONLY through an i8257 DMA controller that the Z80 programs
// and then pulses once a frame. If that emulation is wrong, the failure is
// not a crash and not a blank screen: you get a perfectly good background
// tilemap with no Mario, no barrels and no Kong on it. That is a screen
// which looks like "the renderer works, something else is broken" and sends
// you looking in the wrong file. `--dma` reports transfers and bytes moved
// per second, plus the peak sprites selected on any one scanline, so the
// question is answered with a number instead of an impression -- the same
// lesson as DEVNOTES.md #32 and #38.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#include "dkong_machine.h"
#include "dkong_video.h"
#include "dkong_assets.h"
#include "dkong_input.h"
#include "arcade_hal_video.h"
#include "z80.h"

extern "C" void host_storage_set_rom_dir(const char *dir);

static dkong_system g_system;
static long         g_frame = 0;

// FNV-1a over the emulated machine, for A/B comparisons that must not
// change emulation. Includes the DMA registers and the video latches, so a
// regression in either shows up here.
static uint64_t digest_state(void) {
    uint64_t h = 1469598103934665603ULL;
    const uint8_t *p; size_t n;
    #define FNV(ptr, len) do { p = (const uint8_t *)(ptr); n = (len); \
        for (size_t _i = 0; _i < n; _i++) { h ^= p[_i]; h *= 1099511628211ULL; } } while (0)
    FNV(&g_system.cpu.pc, sizeof(g_system.cpu.pc));
    FNV(&g_system.cpu.sp, sizeof(g_system.cpu.sp));
    FNV(g_system.work_ram, sizeof(g_system.work_ram));
    FNV(g_system.sprite_ram, sizeof(g_system.sprite_ram));
    FNV(g_system.video_ram, sizeof(g_system.video_ram));
    FNV(&g_system.dma, sizeof(g_system.dma));
    FNV(&g_system.flip_screen, sizeof(g_system.flip_screen));
    FNV(&g_system.sprite_bank, sizeof(g_system.sprite_bank));
    FNV(&g_system.palette_bank, sizeof(g_system.palette_bank));
    FNV(&g_system.nmi_mask, sizeof(g_system.nmi_mask));
    #undef FNV
    return h;
}

// Only the first half of each scanline buffer is displayed -- libdvi's
// 16bpp path encodes h_active_pixels/2 source pixels across the full line.
// See tools/README.md; this doubles so the dump looks like the monitor.
#define VISIBLE_SRC_WIDTH (HAL_VIDEO_WIDTH / 2u)

static void dump_ppm(const char *path) {
    static uint16_t row[4096];
    FILE *fp = fopen(path, "wb");
    if (!fp) { fprintf(stderr, "cannot write %s\n", path); return; }
    fprintf(fp, "P6\n%u %u\n255\n", HAL_VIDEO_WIDTH, HAL_VIDEO_HEIGHT);
    for (uint32_t y = 0; y < HAL_VIDEO_HEIGHT; y++) {
        memset(row, 0, sizeof(uint16_t) * HAL_VIDEO_WIDTH);
        dkong_video_render_scanline(&g_system, y, row);
        for (uint32_t x = 0; x < HAL_VIDEO_WIDTH; x++) {
            uint16_t c = row[x / 2u < VISIBLE_SRC_WIDTH ? x / 2u : VISIBLE_SRC_WIDTH - 1u];
            fputc((int)(((c >> 11) & 0x1F) * 255 / 31), fp);
            fputc((int)(((c >>  5) & 0x3F) * 255 / 63), fp);
            fputc((int)(( c        & 0x1F) * 255 / 31), fp);
        }
    }
    fclose(fp);
    printf("[wrote %s at frame %ld]\n", path, g_frame);
}

static void print_state(const char *tag) {
    printf("--- %s (frame %ld) ---\n", tag, g_frame);
    printf("  cpu pc=%04X sp=%04X cyc=%" PRIu32 "  nmi_mask=%d\n",
           g_system.cpu.pc, g_system.cpu.sp, g_system.cpu.cyc,
           (int)g_system.nmi_mask);
    printf("  flip=%d sprite_bank=%u palette_bank=%u  rotation=%u mirror=%d\n",
           (int)g_system.flip_screen, (unsigned)g_system.sprite_bank,
           (unsigned)g_system.palette_bank, (unsigned)g_system.rotation,
           (int)g_system.mirror_x);
    printf("  digest=%016" PRIX64 "\n", digest_state());
}

static void report_dma(long frames_since) {
    uint32_t transfers = 0, bytes = 0, peak = 0, limit_hits = 0;
    dkong_debug_take_dma_stats(&transfers, &bytes);
    dkong_video_debug_take_sprite_stats(&peak, &limit_hits);
    printf("--- dma/sprites over the last %ld frames (frame %ld) ---\n", frames_since, g_frame);
    printf("  8257 transfers: %" PRIu32 "  bytes moved: %" PRIu32 "\n", transfers, bytes);
    printf("  peak sprites on one scanline: %" PRIu32 "   16-limit hit: %" PRIu32 " times\n",
           peak, limit_hits);
    if (transfers == 0)
        printf("    *** ZERO DMA transfers: sprite RAM is never written, so the\n"
               "        screen will show a background and nothing else. Either the\n"
               "        ROM has not reached its sprite code, or the 0x7D85 trigger\n"
               "        or the 8257 register decode is wrong. ***\n");
}

// Scripted input. `jump` is Donkey Kong's BUTTON1.
struct InputEvent { long frame; int btn; };
static InputEvent g_events[64];
static int        g_event_n = 0;
static long       g_press_frames = 6;
static const char *BTNNAME[8] = {"coin","start1","start2","up","down","left","right","jump"};

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
        for (int b = 0; b < 8; b++)
            if (!strcmp(name, BTNNAME[b])) { g_events[g_event_n].frame = fr;
                                             g_events[g_event_n].btn = b; g_event_n++; break; }
    }
}

static const char *find_rom_dir(const char *explicit_dir) {
    static const char *cands[] = {
        NULL, NULL,
        "dkong_assets/rom", "../dkong_assets/rom", "../../dkong_assets/rom",
        "../../../dkong_assets/rom", "../../../../dkong_assets/rom",
    };
    cands[0] = explicit_dir;
    cands[1] = getenv("DKONG_ROM_DIR");
    for (unsigned i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) {
        if (!cands[i]) continue;
        char probe[2048];
        snprintf(probe, sizeof(probe), "%s/c_5et_g.bin", cands[i]);
        FILE *fp = fopen(probe, "rb");
        if (fp) { fclose(fp); return cands[i]; }
    }
    return NULL;
}

static void usage(const char *argv0) {
    printf("usage: %s [options]\n"
           "  --rom DIR       ROM directory (default: search for dkong_assets/rom,\n"
           "                  or $DKONG_ROM_DIR)\n"
           "  --rotation N    override the machine's default screen rotation\n"
           "                  (0=landscape 1=90 CCW 2=180 3=90 CW)\n"
           "  --frames N      frames to run (default 3000)\n"
           "  --seed-cyc N    set the Z80 cycle counter to N right after init, to\n"
           "                  reach the ~23-minute wraparound in seconds (DEVNOTES #22)\n"
           "  --every N       print a state block every N frames (default 0 = off)\n"
           "  --digest-every N  print frame+digest every N frames\n"
           "  --dma           report 8257 transfers and sprite-per-scanline peaks\n"
           "                  (with --every, and once at exit)\n"
           "  --ppm-every N   dump a rendered PPM every N frames\n"
           "  --ppm-prefix P  filename prefix for PPM dumps (default \"frame\")\n"
           "  --stall N       exit 3 if the rendered frame is byte-identical for N\n"
           "                  consecutive checks (default 0 = off; needs --ppm-every)\n"
           "  --input SPEC    scripted presses, e.g. 600:coin,800:start1,900:jump\n"
           "                  (buttons: coin start1 start2 up down left right jump)\n"
           "  --press-frames N  how long each scripted press is held (default 6)\n",
           argv0);
}

int main(int argc, char **argv) {
    const char *rom_arg = NULL, *ppm_prefix = "frame";
    long frames = 3000, every = 0, digest_every = 0, ppm_every = 0, stall_lim = 0;
    long rotation = -1;
    unsigned long long seed_cyc = 0;
    bool do_seed = false, want_dma = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc)               rom_arg = argv[++i];
        else if (!strcmp(argv[i], "--rotation") && i + 1 < argc)     rotation = atol(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)       frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--every") && i + 1 < argc)        every = atol(argv[++i]);
        else if (!strcmp(argv[i], "--digest-every") && i + 1 < argc) digest_every = atol(argv[++i]);
        else if (!strcmp(argv[i], "--dma"))                          want_dma = true;
        else if (!strcmp(argv[i], "--ppm-every") && i + 1 < argc)    ppm_every = atol(argv[++i]);
        else if (!strcmp(argv[i], "--ppm-prefix") && i + 1 < argc)   ppm_prefix = argv[++i];
        else if (!strcmp(argv[i], "--stall") && i + 1 < argc)        stall_lim = atol(argv[++i]);
        else if (!strcmp(argv[i], "--press-frames") && i + 1 < argc) g_press_frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--input") && i + 1 < argc)        parse_events(argv[++i]);
        else if (!strcmp(argv[i], "--seed-cyc") && i + 1 < argc)     { seed_cyc = strtoull(argv[++i], NULL, 0); do_seed = true; }
        else { usage(argv[0]); return 2; }
    }

    const char *rom_dir = find_rom_dir(rom_arg);
    if (!rom_dir) {
        fprintf(stderr, "error: could not locate a Donkey Kong ROM directory "
                        "(looked for c_5et_g.bin). Use --rom DIR.\n");
        return 1;
    }
    printf("rom dir: %s\n", rom_dir);
    host_storage_set_rom_dir(rom_dir);

    dkong_init(&g_system);
    if (rotation >= 0 && rotation <= 3) g_system.rotation = (uint8_t)rotation;

    uint16_t err = 0;
    if (!dkong_load_assets(&g_system, &err)) {
        fprintf(stderr, "error: dkong_load_assets failed (error color %04X: %s)\n",
                err, err == DKONG_COLOR_ERROR_NO_CARD ? "no storage" : "missing ROM files");
        return 1;
    }

    if (do_seed) {
        g_system.cpu.cyc = (uint32_t)seed_cyc;
        printf("seeded cpu.cyc = %" PRIu32 " (wraps in ~%.1f frames)\n",
               g_system.cpu.cyc,
               (double)(4294967296.0 - (double)g_system.cpu.cyc) / 50688.0);
    }

    printf("running %ld frames...\n\n", frames);

    static uint16_t prev_row[4096], cur_row[4096];
    long identical_streak = 0;
    long last_report = 0;

    for (g_frame = 0; g_frame < frames; g_frame++) {
        dkong_input_update(&g_system,
                           btn_active(0, g_frame),  // coin
                           btn_active(1, g_frame),  // start1
                           btn_active(2, g_frame),  // start2
                           btn_active(3, g_frame),  // up
                           btn_active(4, g_frame),  // down
                           btn_active(5, g_frame),  // left
                           btn_active(6, g_frame),  // right
                           btn_active(7, g_frame),  // jump
                           false, false);           // rotate/mirror meta
        dkong_run_frame(&g_system);

        if (every > 0 && (g_frame % every) == 0) {
            print_state("state");
            if (want_dma) { report_dma(g_frame - last_report); last_report = g_frame; }
        }
        if (digest_every > 0 && (g_frame % digest_every) == 0)
            printf("frame %6ld digest=%016" PRIX64 "\n", g_frame, digest_state());

        if (ppm_every > 0 && (g_frame % ppm_every) == 0) {
            char path[1024];
            snprintf(path, sizeof(path), "%s_%05ld.ppm", ppm_prefix, g_frame);
            dump_ppm(path);

            if (stall_lim > 0) {
                memcpy(prev_row, cur_row, sizeof(cur_row));
                memset(cur_row, 0, sizeof(cur_row));
                dkong_video_render_scanline(&g_system, HAL_VIDEO_HEIGHT / 2, cur_row);
                if (g_frame > 0 && memcmp(prev_row, cur_row, sizeof(cur_row)) == 0) {
                    if (++identical_streak >= stall_lim) {
                        printf("\n*** STALLED: rendered scanline unchanged for %ld checks ***\n",
                               identical_streak);
                        print_state("stall");
                        if (want_dma) report_dma(g_frame - last_report);
                        return 3;
                    }
                } else identical_streak = 0;
            }
        }
    }

    print_state("final");
    if (want_dma) report_dma(g_frame - last_report);
    return 0;
}
