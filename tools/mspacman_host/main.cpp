// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Host-harness driver for ArcadeMachine_MsPacman -- see ../README.md.
//
// Same idea as pacman_host: runs the REAL Ms. Pac-Man machine (the actual
// Z80 core, real ROMs, the real aux-board decode and bank switching, real
// port decode and per-scanline frame interleaving) natively against the
// shared stub ArcadeHAL in ../host_common/.
//
// The reason THIS one exists is `--banks`. Ms. Pac-Man's aux daughterboard
// is the whole difference between this machine and Pac-Man, and it is
// almost entirely invisible from the outside: if the decode is wrong or the
// bank switching never fires, the most likely outcome is not a crash but
// **plain Pac-Man**, or Pac-Man with forty 8-byte holes in it. Those look
// like a working port to anyone glancing at a screenshot of the attract
// mode, which is exactly the failure DEVNOTES.md #32 warns about -- a
// symptom that could plausibly be authentic still deserves a measurement.
// `--banks` counts the switches and reports which ranges triggered them, so
// "is the aux board actually doing anything" is a number rather than an
// impression.
//
// It changes no library source to do it: ArcadeCPU_Z80 wires memory access
// through per-instance function pointers on the z80 struct, so the harness
// captures those after init and substitutes counting wrappers that delegate
// to the originals -- the same trick galaga_host's --watch uses.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#include "mspacman_machine.h"
#include "mspacman_video.h"
#include "mspacman_assets.h"
#include "mspacman_input.h"
#include "arcade_hal_video.h"
#include "z80.h"

extern "C" void host_storage_set_rom_dir(const char *dir);

static mspacman_system g_system;
static long            g_frame = 0;

// --- bank-switch instrumentation (wrappers, no library changes) ----------

static uint8_t (*g_orig_read)(void *, uint16_t)         = NULL;
static void    (*g_orig_write)(void *, uint16_t, uint8_t) = NULL;

// The eight trigger ranges, from MAME's mspacman_map(). Kept here as the
// harness's OWN copy on purpose: if this list and mspacman_ports.cpp ever
// disagree, --banks reports switches attributed to "other", which is a
// louder failure than silently agreeing with a bug.
static const struct { uint16_t base; const char *name; } TRIGGERS[] = {
    { 0x0038, "0038 dis" }, { 0x03b0, "03b0 dis" }, { 0x1600, "1600 dis" },
    { 0x2120, "2120 dis" }, { 0x3ff0, "3ff0 dis" }, { 0x3ff8, "3ff8 ENA" },
    { 0x8000, "8000 dis" }, { 0x97f0, "97f0 dis" },
};
#define NTRIG ((int)(sizeof(TRIGGERS)/sizeof(TRIGGERS[0])))

static uint64_t g_trig_hits[NTRIG];
static uint64_t g_trig_other;   // a switch at an address not in the list above
static uint64_t g_switches;     // bank actually CHANGED value
static uint64_t g_frames_in[2]; // frames ending with each bank selected

static void note_access(uint16_t addr, uint8_t before) {
    if (g_system.bank == before) return; // no switch
    g_switches++;
    for (int i = 0; i < NTRIG; i++)
        if ((addr & 0xfff8u) == TRIGGERS[i].base) { g_trig_hits[i]++; return; }
    g_trig_other++;
}

static uint8_t counting_read(void *ud, uint16_t addr) {
    uint8_t before = g_system.bank;
    uint8_t v = g_orig_read(ud, addr);
    note_access(addr, before);
    return v;
}

static void counting_write(void *ud, uint16_t addr, uint8_t data) {
    uint8_t before = g_system.bank;
    g_orig_write(ud, addr, data);
    note_access(addr, before);
}

static void install_bank_counters(void) {
    g_orig_read  = g_system.cpu.read_byte;
    g_orig_write = g_system.cpu.write_byte;
    g_system.cpu.read_byte  = counting_read;
    g_system.cpu.write_byte = counting_write;
}

static void report_banks(void) {
    printf("--- bank switching (frame %ld) ---\n", g_frame);
    printf("  total switches: %" PRIu64 "   current bank: %u (%s)\n",
           g_switches, (unsigned)g_system.bank,
           g_system.bank == MSPACMAN_BANK_DECRYPTED ? "decrypted/Ms." : "plain/Pac-Man");
    printf("  frames ending in: decrypted=%" PRIu64 "  plain=%" PRIu64 "\n",
           g_frames_in[MSPACMAN_BANK_DECRYPTED], g_frames_in[MSPACMAN_BANK_PLAIN]);
    for (int i = 0; i < NTRIG; i++)
        if (g_trig_hits[i])
            printf("    %s : %" PRIu64 "\n", TRIGGERS[i].name, g_trig_hits[i]);
    if (g_trig_other)
        printf("    *** %" PRIu64 " switches at addresses NOT in this harness's"
               " trigger list -- harness and mspacman_ports.cpp disagree ***\n", g_trig_other);
    if (g_switches == 0)
        printf("    *** ZERO switches: the aux board is inert. Either the ROM never\n"
               "        reached a trigger range, or bank_trigger() is not matching. ***\n");
}

// --- state / rendering ---------------------------------------------------

// FNV-1a over the whole emulated machine, for A/B comparisons that must not
// change emulation (see invaders_host's --digest-every, and DEVNOTES #36).
// Includes `bank`, so a decode or banking regression shows up here too.
static uint64_t digest_state(void) {
    uint64_t h = 1469598103934665603ULL;
    const uint8_t *p; size_t n;
    #define FNV(ptr, len) do { p = (const uint8_t *)(ptr); n = (len); \
        for (size_t _i = 0; _i < n; _i++) { h ^= p[_i]; h *= 1099511628211ULL; } } while (0)
    FNV(&g_system.cpu.pc, sizeof(g_system.cpu.pc));
    FNV(&g_system.cpu.sp, sizeof(g_system.cpu.sp));
    FNV(&g_system.bank, sizeof(g_system.bank));
    FNV(g_system.video_ram, sizeof(g_system.video_ram));
    FNV(g_system.color_ram, sizeof(g_system.color_ram));
    FNV(g_system.work_ram, sizeof(g_system.work_ram));
    FNV(g_system.sprite_num, sizeof(g_system.sprite_num));
    FNV(g_system.sprite_pos, sizeof(g_system.sprite_pos));
    #undef FNV
    return h;
}

// Only the first half of each scanline buffer is displayed -- libdvi's 16bpp
// path encodes h_active_pixels/2 source pixels across the full line, which is
// why every renderer here targets a 320-wide visible axis. See
// tools/README.md; this doubles so the dump looks like the monitor.
#define VISIBLE_SRC_WIDTH (HAL_VIDEO_WIDTH / 2u)

static void dump_ppm(const char *path) {
    static uint16_t row[4096];
    FILE *fp = fopen(path, "wb");
    if (!fp) { fprintf(stderr, "cannot write %s\n", path); return; }
    fprintf(fp, "P6\n%u %u\n255\n", HAL_VIDEO_WIDTH, HAL_VIDEO_HEIGHT);
    for (uint32_t y = 0; y < HAL_VIDEO_HEIGHT; y++) {
        memset(row, 0, sizeof(uint16_t) * HAL_VIDEO_WIDTH);
        mspacman_video_render_scanline(&g_system, y, row);
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
    printf("  cpu pc=%04X sp=%04X cyc=%" PRIu32 "  bank=%u\n",
           g_system.cpu.pc, g_system.cpu.sp, g_system.cpu.cyc,
           (unsigned)g_system.bank);
    printf("  rotation=%u mirror=%d  digest=%016" PRIX64 "\n",
           (unsigned)g_system.rotation, (int)g_system.mirror_x, digest_state());
}

// --- scripted input ------------------------------------------------------

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

// Probes for "u5" rather than a Pac-Man chip name: pacman.6e/6f/6h/6j are in
// BOTH sets and byte-identical, so probing one of those would happily accept
// a plain Pac-Man ROM directory and then fail deep inside the decode.
static const char *find_rom_dir(const char *explicit_dir) {
    static const char *cands[] = {
        NULL, NULL,
        "mspacman_assets/rom", "../mspacman_assets/rom", "../../mspacman_assets/rom",
        "../../../mspacman_assets/rom", "../../../../mspacman_assets/rom",
    };
    cands[0] = explicit_dir;
    cands[1] = getenv("MSPACMAN_ROM_DIR");
    for (unsigned i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) {
        if (!cands[i]) continue;
        char probe[2048];
        snprintf(probe, sizeof(probe), "%s/u5", cands[i]);
        FILE *fp = fopen(probe, "rb");
        if (fp) { fclose(fp); return cands[i]; }
    }
    return NULL;
}

static void usage(const char *argv0) {
    printf("usage: %s [options]\n"
           "  --rom DIR       ROM directory (default: search for mspacman_assets/rom,\n"
           "                  or $MSPACMAN_ROM_DIR). Probed by the presence of \"u5\",\n"
           "                  since the Pac-Man chips alone would not distinguish a\n"
           "                  plain Pac-Man set from this one.\n"
           "  --rotation N    override the machine's default screen rotation\n"
           "                  (0=landscape 1=90 CCW 2=180 3=90 CW)\n"
           "  --frames N      frames to run (default 3000)\n"
           "  --seed-cyc N    set the Z80 cycle counter to N right after init, to\n"
           "                  reach the ~23-minute wraparound in seconds (DEVNOTES #22)\n"
           "  --every N       print a state block every N frames (default 0 = off)\n"
           "  --digest-every N  print frame+digest every N frames\n"
           "  --banks         print a bank-switching report at exit (and with --every)\n"
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
    long frames = 3000, every = 0, digest_every = 0, ppm_every = 0, stall_lim = 0;
    long rotation = -1;
    unsigned long long seed_cyc = 0;
    bool do_seed = false, want_banks = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc)               rom_arg = argv[++i];
        else if (!strcmp(argv[i], "--rotation") && i + 1 < argc)     rotation = atol(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)       frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--every") && i + 1 < argc)        every = atol(argv[++i]);
        else if (!strcmp(argv[i], "--digest-every") && i + 1 < argc) digest_every = atol(argv[++i]);
        else if (!strcmp(argv[i], "--banks"))                        want_banks = true;
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
        fprintf(stderr, "error: could not locate a Ms. Pac-Man ROM directory "
                        "(looked for u5). Use --rom DIR.\n");
        return 1;
    }
    printf("rom dir: %s\n", rom_dir);
    host_storage_set_rom_dir(rom_dir);

    mspacman_init(&g_system);
    if (rotation >= 0 && rotation <= 3) g_system.rotation = (uint8_t)rotation;

    uint16_t err = 0;
    if (!mspacman_load_assets(&g_system, &err)) {
        fprintf(stderr, "error: mspacman_load_assets failed (error color %04X: %s)\n",
                err, err == MSPACMAN_COLOR_ERROR_NO_CARD ? "no storage"
                                                         : "missing ROM files");
        return 1;
    }

    // After init/load so the wrappers sit on top of the real handlers, and
    // before any cycles run so no switch is missed.
    install_bank_counters();

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
        mspacman_input_update(&g_system,
                              btn_active(0, g_frame),  // coin
                              btn_active(1, g_frame),  // start1
                              btn_active(2, g_frame),  // start2
                              btn_active(3, g_frame),  // up
                              btn_active(4, g_frame),  // down
                              btn_active(5, g_frame),  // left
                              btn_active(6, g_frame),  // right
                              false, false);           // rotate/mirror meta
        mspacman_run_frame(&g_system);
        g_frames_in[g_system.bank & 1]++;

        if (every > 0 && (g_frame % every) == 0) {
            print_state("state");
            if (want_banks) report_banks();
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
                mspacman_video_render_scanline(&g_system, HAL_VIDEO_HEIGHT / 2, cur_row);
                if (g_frame > 0 && memcmp(prev_row, cur_row, sizeof(cur_row)) == 0) {
                    if (++identical_streak >= stall_lim) {
                        printf("\n*** STALLED: rendered scanline unchanged for %ld checks ***\n",
                               identical_streak);
                        print_state("stall");
                        if (want_banks) report_banks();
                        return 3;
                    }
                } else identical_streak = 0;
            }
        }
    }

    print_state("final");
    if (want_banks) report_banks();
    return 0;
}
