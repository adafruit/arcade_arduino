// Host-harness driver for ArcadeMachine_Galaga -- see README.md.
//
// Runs the real Galaga machine (all 3 Z80 cores, real ROMs, real port
// decode, real per-scanline frame interleaving) on the host, and traces
// every access to a chosen set of shared-RAM addresses in execution order.
//
// The tracer is NON-INVASIVE: it does not touch ArcadeMachine_Galaga's
// sources at all. SAMP wires each CPU's memory access through per-instance
// function pointers on the z80 struct (z80.h's read_byte/write_byte/
// userdata design), so after galaga_init() has wired them, the harness can
// simply capture those pointers and substitute wrappers that log and then
// delegate. Nothing in the library knows or cares.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h> // PRIu32: cycle counters are uint32_t (see z80.h)

#include "galaga_machine.h"
#include "galaga_ports.h"
#include "galaga_video.h"
#include "galaga_assets.h"
#include "galaga_input.h"
#include "galaga_audio.h"
#include "arcade_hal_video.h" // HAL_VIDEO_WIDTH/HEIGHT, for the PPM dump
#include "z80.h"

extern "C" void host_storage_set_rom_dir(const char *dir);
extern "C" void host_audio_fill(int32_t *out, int count);

galaga_system g_system;

// --- watch list ----------------------------------------------------------

static uint16_t g_watch[64];
static int      g_watch_n = 0;

static inline bool is_watched(uint16_t a) {
    for (int i = 0; i < g_watch_n; i++) if (g_watch[i] == a) return true;
    return false;
}

// --- trace ---------------------------------------------------------------

static const char *CPUNAME[3] = { "MAIN", "SUB ", "SUB2" };
static z80 *g_cpu[3];

typedef uint8_t (*rd_fn)(void *, uint16_t);
typedef void    (*wr_fn)(void *, uint16_t, uint8_t);
static rd_fn g_orig_rd[3];
static wr_fn g_orig_wr[3];

static long               g_frame    = 0;
static unsigned long long g_distinct = 0; // count of distinct (non-repeat) events
static bool               g_quiet    = false;
static bool               g_show_cyc = false;

// Run-length collapsing is essential, not cosmetic: both deadlock
// candidates here are tight poll loops (main re-reads 0x92A0 forever, sub
// re-reads 0x9100 forever), which would otherwise emit millions of
// identical lines and bury the handful of real state transitions.
struct Ev {
    int      cpu;
    char     rw;
    uint16_t addr;
    uint8_t  val;
    uint16_t pc;
    long     frame;
    unsigned long long n;
    uint32_t cyc[3]; // matches z80.h's uint32_t cyc (device-width)
    bool     valid;
};
static Ev g_pend;

static void flush_pending(void) {
    if (!g_pend.valid) return;
    if (!g_quiet) {
        printf("f%-6ld %s %c %04X = %02X  pc=%04X",
               g_pend.frame, CPUNAME[g_pend.cpu], g_pend.rw,
               g_pend.addr, g_pend.val, g_pend.pc);
        if (g_show_cyc)
            printf("  cyc=%" PRIu32 "/%" PRIu32 "/%" PRIu32, g_pend.cyc[0], g_pend.cyc[1], g_pend.cyc[2]);
        if (g_pend.n > 1) printf("   x%llu", g_pend.n);
        printf("\n");
    }
    g_pend.valid = false;
}

static void log_ev(int cpu, char rw, uint16_t addr, uint8_t val, uint16_t pc) {
    if (g_pend.valid && g_pend.cpu == cpu && g_pend.rw == rw &&
        g_pend.addr == addr && g_pend.val == val && g_pend.pc == pc) {
        g_pend.n++;
        return;
    }
    flush_pending();
    g_pend.cpu = cpu; g_pend.rw = rw; g_pend.addr = addr; g_pend.val = val;
    g_pend.pc = pc;   g_pend.frame = g_frame; g_pend.n = 1; g_pend.valid = true;
    g_pend.cyc[0] = g_system.cpu_main.cyc;
    g_pend.cyc[1] = g_system.cpu_sub.cyc;
    g_pend.cyc[2] = g_system.cpu_sub2.cyc;
    g_distinct++;
}

// pc is sampled as-is: mid-instruction the Z80 core has already advanced
// it past the opcode (and any operand bytes fetched so far), so treat it
// as "just after the accessing instruction started", not its exact start.
template <int CPU> static uint8_t trace_rd(void *ud, uint16_t addr) {
    uint8_t v = g_orig_rd[CPU](ud, addr);
    if (is_watched(addr)) log_ev(CPU, 'R', addr, v, g_cpu[CPU]->pc);
    return v;
}
template <int CPU> static void trace_wr(void *ud, uint16_t addr, uint8_t val) {
    if (is_watched(addr)) log_ev(CPU, 'W', addr, val, g_cpu[CPU]->pc);
    g_orig_wr[CPU](ud, addr, val);
}

static void install_tracers(void) {
    g_cpu[0] = &g_system.cpu_main;
    g_cpu[1] = &g_system.cpu_sub;
    g_cpu[2] = &g_system.cpu_sub2;
    for (int i = 0; i < 3; i++) {
        g_orig_rd[i] = g_cpu[i]->read_byte;
        g_orig_wr[i] = g_cpu[i]->write_byte;
    }
    g_system.cpu_main.read_byte  = trace_rd<0>;
    g_system.cpu_main.write_byte = trace_wr<0>;
    g_system.cpu_sub.read_byte   = trace_rd<1>;
    g_system.cpu_sub.write_byte  = trace_wr<1>;
    g_system.cpu_sub2.read_byte  = trace_rd<2>;
    g_system.cpu_sub2.write_byte = trace_wr<2>;
}

// --- helpers -------------------------------------------------------------

// Renders a full frame through the REAL renderer (galaga_video_render_scanline,
// the same function the device calls) and writes it as a PPM. Lets the whole
// boot sequence be inspected visually with no hardware, no camera, and no
// guessing at what a photographed screen is showing.
static void dump_ppm(const char *path) {
    static uint16_t row[4096];
    FILE *fp = fopen(path, "wb");
    if (!fp) { fprintf(stderr, "cannot write %s\n", path); return; }
    fprintf(fp, "P6\n%u %u\n255\n", HAL_VIDEO_WIDTH, HAL_VIDEO_HEIGHT);
    for (uint32_t y = 0; y < HAL_VIDEO_HEIGHT; y++) {
        memset(row, 0, sizeof(uint16_t) * HAL_VIDEO_WIDTH);
        galaga_video_render_scanline(&g_system, y, row);
        for (uint32_t x = 0; x < HAL_VIDEO_WIDTH; x++) {
            uint16_t c = row[x];
            fputc((int)(((c >> 11) & 0x1F) * 255 / 31), fp);
            fputc((int)(((c >>  5) & 0x3F) * 255 / 63), fp);
            fputc((int)(( c        & 0x1F) * 255 / 31), fp);
        }
    }
    fclose(fp);
    printf("[wrote %s at frame %ld]\n", path, g_frame);
}


// Sprite census -- the number of on-screen objects of a given kind, per
// frame. Written to answer "how many bullets did that press actually
// fire?", which whole-frame PPM comparison cannot: two runs can differ for
// reasons that have nothing to do with the thing under test, and can match
// while still both firing. Sprite RAM layout is galaga_video.cpp's
// (spriteram/_2/_3 = the last 0x80 bytes of ram1/2/3), quoted from
// galaga_v.cpp's draw_sprites().
static void sprite_census(long frame, int want_code) {
    const uint8_t *s1 = &g_system.ram1[0x380];
    const uint8_t *s2 = &g_system.ram2[0x380];
    const uint8_t *s3 = &g_system.ram3[0x380];
    printf("f%-6ld sprites:", frame);
    int matched = 0;
    for (int offs = 0; offs < 0x80; offs += 2) {
        int sizey = (s3[offs] >> 3) & 0x01;
        int sy = 256 - s2[offs] + 1;
        sy -= 16 * sizey;
        sy = (sy & 0xFF) - 32;
        int height = 16 * (sizey + 1);
        if (sy >= 224 || sy + height <= 0) continue;   // off screen
        int code  = s1[offs] & 0x7F;
        int sx    = s2[offs + 1] - 40 + 0x100 * (s3[offs + 1] & 3);
        if (want_code >= 0) {
            if (code == want_code) { matched++; printf(" [%d,%d]", sx, sy); }
        } else {
            printf(" %02X@%d,%d", code, sx, sy);
        }
    }
    if (want_code >= 0) printf("  count(code %02X)=%d", want_code, matched);
    printf("\n");
}

// --- WAV capture ------------------------------------------------------
//
// Pulls the machine's own audio fill callback once per emulated frame --
// the same callback the board's audio ISR drives on hardware -- and writes
// the result as a 16-bit mono WAV. Lets the WSG synthesis be *listened to*
// before ever flashing, which beats debugging silence on a device where
// the only feedback is "no sound".
static FILE     *g_wav = NULL;
static uint32_t  g_wav_samples = 0;

static void wav_put32(FILE *f, uint32_t v) { fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f);
                                             fputc((v >> 16) & 0xFF, f); fputc((v >> 24) & 0xFF, f); }
static void wav_put16(FILE *f, uint16_t v) { fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f); }

static void wav_open(const char *path) {
    g_wav = fopen(path, "wb");
    if (!g_wav) { fprintf(stderr, "cannot write %s\n", path); return; }
    fwrite("RIFF", 1, 4, g_wav); wav_put32(g_wav, 0); // sizes patched on close
    fwrite("WAVEfmt ", 1, 8, g_wav);
    wav_put32(g_wav, 16); wav_put16(g_wav, 1); wav_put16(g_wav, 1);
    wav_put32(g_wav, GALAGA_AUDIO_SAMPLE_RATE);
    wav_put32(g_wav, GALAGA_AUDIO_SAMPLE_RATE * 2);
    wav_put16(g_wav, 2); wav_put16(g_wav, 16);
    fwrite("data", 1, 4, g_wav); wav_put32(g_wav, 0);
}

// One frame's worth of samples at 60Hz. hal_audio's callback hands back
// packed stereo (same 16-bit sample in both halves); we keep the low half.
// Always pumps the machine's audio callback, whether or not a WAV is being
// written -- on hardware the board's audio ISR runs regardless, and the
// callback has side effects the machine depends on (it is where the 54XX
// play-command handover happens). Making this conditional on --wav once
// hid the 54XX entirely from every diagnostic run that omitted the flag.
static void wav_pump_frame(void) {
    enum { N = GALAGA_AUDIO_SAMPLE_RATE / 60 };
    static int32_t buf[N];
    memset(buf, 0, sizeof(buf));
    host_audio_fill(buf, N);
    if (!g_wav) return;
    for (int i = 0; i < N; i++) wav_put16(g_wav, (uint16_t)(buf[i] & 0xFFFF));
    g_wav_samples += N;
}

static void wav_close(void) {
    if (!g_wav) return;
    uint32_t data_bytes = g_wav_samples * 2;
    fseek(g_wav, 4, SEEK_SET);  wav_put32(g_wav, 36 + data_bytes);
    fseek(g_wav, 40, SEEK_SET); wav_put32(g_wav, data_bytes);
    fclose(g_wav); g_wav = NULL;
    printf("[wrote %u samples of audio (%.1fs)]\n",
           g_wav_samples, (double)g_wav_samples / GALAGA_AUDIO_SAMPLE_RATE);
}

static uint8_t shared_peek(uint16_t addr) {
    if (addr >= 0x8000 && addr < 0x8800) return g_system.video_ram[addr - 0x8000];
    if (addr >= 0x8800 && addr < 0x8C00) return g_system.ram1[addr - 0x8800];
    if (addr >= 0x9000 && addr < 0x9400) return g_system.ram2[addr - 0x9000];
    if (addr >= 0x9800 && addr < 0x9C00) return g_system.ram3[addr - 0x9800];
    return 0xFF;
}

static void print_state(const char *tag) {
    printf("--- %s (frame %ld) ---\n", tag, g_frame);
    printf("  main pc=%04X sp=%04X cyc=%" PRIu32 "\n",
           g_system.cpu_main.pc, g_system.cpu_main.sp, g_system.cpu_main.cyc);
    printf("  sub  pc=%04X sp=%04X cyc=%" PRIu32 "\n",
           g_system.cpu_sub.pc, g_system.cpu_sub.sp, g_system.cpu_sub.cyc);
    printf("  sub2 pc=%04X sp=%04X cyc=%" PRIu32 "\n",
           g_system.cpu_sub2.pc, g_system.cpu_sub2.sp, g_system.cpu_sub2.cyc);
    printf("  watched:");
    for (int i = 0; i < g_watch_n; i++)
        printf(" %04X=%02X", g_watch[i], shared_peek(g_watch[i]));
    printf("\n");
    printf("  irq1=%d irq2=%d nmi2=%d sub_reset_released=%d\n",
           (int)g_system.irq1_enable, (int)g_system.irq2_enable,
           (int)g_system.nmi2_enable, (int)g_system.sub_reset_released);
    // Starfield (05XX) videolatch bits, decoded -- the only way to see what
    // the game is actually asking the star hardware for. speed is Q2..Q0,
    // sets are Q3 and Q4|2, en is Q5 (_STARCLR). See galaga_video.cpp.
    {
        uint8_t sc = g_system.starfield_control;
        printf("  starfield ctl=%02X  speed=%u sets=%u,%u en=%u\n",
               sc, (unsigned)(sc & 7u), (unsigned)((sc >> 3) & 1u),
               (unsigned)(((sc >> 4) & 1u) | 2u), (unsigned)((sc >> 5) & 1u));
    }
    printf("  ram-test pass=%u fail=%u   iff1 m/s/2=%d/%d/%d  im m/s/2=%d/%d/%d\n",
           g_system.debug_checksum_pass, g_system.debug_checksum_fail,
           (int)g_system.cpu_main.iff1, (int)g_system.cpu_sub.iff1,
           (int)g_system.cpu_sub2.iff1,
           (int)g_system.cpu_main.interrupt_mode,
           (int)g_system.cpu_sub.interrupt_mode,
           (int)g_system.cpu_sub2.interrupt_mode);
}

static const char *find_rom_dir(const char *explicit_dir) {
    static const char *cands[] = {
        NULL, NULL,
        "galaga_assets/rom",
        "../galaga_assets/rom",
        "../../galaga_assets/rom",
        "../../../galaga_assets/rom",
        "../../../../galaga_assets/rom",
    };
    cands[0] = explicit_dir;
    cands[1] = getenv("GALAGA_ROM_DIR");
    for (unsigned i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) {
        if (!cands[i]) continue;
        char probe[2048];
        snprintf(probe, sizeof(probe), "%s/gg1-1b.3p", cands[i]);
        FILE *fp = fopen(probe, "rb");
        if (fp) { fclose(fp); return cands[i]; }
    }
    return NULL;
}

// Scripted input: "frame:button[,frame:button...]", button in
// coin|start1|start2|left|right|fire. Each entry presses that button for
// --press-frames frames starting at that frame. Lets a full coin-up and
// play sequence be exercised on the host, with no hardware and no hands.
struct InputEvent { long frame; int btn; };
static InputEvent g_events[64];
static int        g_event_n = 0;
static long       g_press_frames = 6;
static const char *BTNNAME[6] = {"coin","start1","start2","left","right","fire"};

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
        for (int b = 0; b < 6; b++) {
            if (!strcmp(name, BTNNAME[b])) {
                g_events[g_event_n].frame = fr;
                g_events[g_event_n].btn   = b;
                g_event_n++;
                break;
            }
        }
    }
}

static void usage(const char *argv0) {
    printf("usage: %s [options]\n"
           "  --rom DIR       ROM directory (default: search for galaga_assets/rom,\n"
           "                  or $GALAGA_ROM_DIR)\n"
           "  --rotation N    override the machine's default screen rotation\n"
           "                  (0=landscape 1=90 CCW 2=180 3=90 CW), so an\n"
           "                  orientation can be checked without hardware\n"
           "  --frames N      frames to run (default 3000, ~50s of game time)\n"
           "  --watch a,b,..  hex addresses to trace (default 9100,9101,9102,92A0)\n"
           "  --stall N       declare a stall after N frames with no new distinct\n"
           "                  traced event and no ram-test progress (default 180, 0=off)\n"
           "  --every N       print a state block every N frames (default 0 = off)\n"
           "  --cyc           include per-CPU cycle counts on each trace line\n"
           "  --quiet         suppress the per-event trace (state blocks only)\n"
           "  --input SPEC    scripted button presses, e.g. 1200:coin,1400:start1\n"
           "                  (buttons: coin start1 start2 left right fire)\n"
           "  --press-frames N  how long each scripted press is held (default 6)\n"
           "  --wav FILE      capture the machine's own audio output to a 16-bit WAV\n"
           "  --ppm-every N   dump a rendered frame as a PPM every N frames\n"
           "  --ppm-from N    don't start dumping until frame N. Use with\n"
           "                  --ppm-every 1 to capture a short window of CONSECUTIVE\n"
           "                  frames deep into a run (how the starfield scroll rate\n"
           "                  was measured) without writing ~1MB for every frame\n"
           "                  before it.\n"
           "  --ppm-to N      stop dumping after frame N (0 = no limit)\n"
           "  --census A B    print the on-screen sprite list for frames A..B\n"
           "  --census-code C with --census, count only sprites of code C and\n"
           "                  print their positions -- how you count bullets\n"
           "  --ppm-prefix S  filename prefix for the dumps (default \"frame\")\n"
           "  --seed-cyc N    set all 3 cycle counters to N before any cycles run; use\n"
           "                  a value just under 2^32 to reach the ~23-minute 32-bit\n"
           "                  wraparound in seconds\n",
           argv0);
}

int main(int argc, char **argv) {
    const char *rom_arg    = NULL;
    long        frames     = 3000;
    long        rotation   = -1;
    long        stall_lim  = 180;
    long        every      = 0;
    long        ppm_every  = 0;
    long        ppm_from   = 0;
    long        census_from = -1, census_to = -1, census_code = -1;
    long        ppm_to     = 0;
    unsigned long long seed_cyc = 0;
    const char *wav_path = NULL;
    bool        do_seed    = false;
    const char *ppm_prefix = "frame";

    g_watch[g_watch_n++] = 0x9100;
    g_watch[g_watch_n++] = 0x9101;
    g_watch[g_watch_n++] = 0x9102;
    g_watch[g_watch_n++] = 0x92A0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc)          rom_arg = argv[++i];
        else if (!strcmp(argv[i], "--rotation") && i + 1 < argc) rotation = atol(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)  frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--stall") && i + 1 < argc)   stall_lim = atol(argv[++i]);
        else if (!strcmp(argv[i], "--every") && i + 1 < argc)   every = atol(argv[++i]);
        else if (!strcmp(argv[i], "--ppm-every") && i + 1 < argc) ppm_every = atol(argv[++i]);
        else if (!strcmp(argv[i], "--ppm-from") && i + 1 < argc) ppm_from = atol(argv[++i]);
        else if (!strcmp(argv[i], "--census") && i + 2 < argc) { census_from = atol(argv[++i]); census_to = atol(argv[++i]); }
        else if (!strcmp(argv[i], "--census-code") && i + 1 < argc) census_code = (int)strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--ppm-to") && i + 1 < argc)   ppm_to = atol(argv[++i]);
        else if (!strcmp(argv[i], "--ppm-prefix") && i + 1 < argc) ppm_prefix = argv[++i];
        else if (!strcmp(argv[i], "--input") && i + 1 < argc)    parse_events(argv[++i]);
        else if (!strcmp(argv[i], "--press-frames") && i + 1 < argc) g_press_frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--seed-cyc") && i + 1 < argc) { seed_cyc = strtoull(argv[++i], NULL, 0); do_seed = true; }
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc)      wav_path = argv[++i];
        else if (!strcmp(argv[i], "--cyc"))                     g_show_cyc = true;
        else if (!strcmp(argv[i], "--quiet"))                   g_quiet = true;
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc) {
            g_watch_n = 0;
            char *s = strdup(argv[++i]);
            for (char *tok = strtok(s, ","); tok && g_watch_n < 64; tok = strtok(NULL, ","))
                g_watch[g_watch_n++] = (uint16_t)strtoul(tok, NULL, 16);
            free(s);
        } else { usage(argv[0]); return 2; }
    }

    const char *rom_dir = find_rom_dir(rom_arg);
    if (!rom_dir) {
        fprintf(stderr, "error: could not locate a Galaga ROM directory "
                        "(looked for gg1-1b.3p). Use --rom DIR.\n");
        return 1;
    }
    printf("rom dir: %s\n", rom_dir);
    host_storage_set_rom_dir(rom_dir);

    galaga_init(&g_system);
    // Applied after init, which is where each machine sets its own default
    // (see that machine's *_init). Lets an orientation be checked in the
    // harness rather than by flashing and physically turning a monitor.
    if (rotation >= 0 && rotation <= 3) g_system.rotation = (uint8_t)rotation;

    uint16_t err = 0;
    if (!galaga_load_assets(&g_system, &err)) {
        fprintf(stderr, "error: galaga_load_assets failed (error color %04X)\n", err);
        return 1;
    }

    // Seed all three cycle counters BEFORE any cycles run, so everything the
    // machine derives from them (namco_busy_until, io06_nmi_next,
    // reset_release_main_cyc) is computed relative to this base. Use a value
    // just under 2^32 to reach the 32-bit wraparound in seconds rather than
    // the ~23 minutes it takes in real time -- that wrap is exactly where a
    // freeze bug hid for a long time (see interleave_to_target()).
    if (do_seed) {
        g_system.cpu_main.cyc = (uint32_t)seed_cyc;
        g_system.cpu_sub.cyc  = (uint32_t)seed_cyc;
        g_system.cpu_sub2.cyc = (uint32_t)seed_cyc;
        printf("seeded all 3 cyc counters = %" PRIu32 " (wraps in ~%.1f frames)\n",
               g_system.cpu_main.cyc,
               (4294967296.0 - (double)g_system.cpu_main.cyc) / 50688.0);
    }

    install_tracers(); // after wiring, before any CPU steps

    printf("watching:");
    for (int i = 0; i < g_watch_n; i++) printf(" %04X", g_watch[i]);
    if (wav_path) wav_open(wav_path);
    printf("\nrunning %ld frames...\n\n", frames);

    unsigned long long last_distinct = ~0ull;
    uint32_t           last_pass     = ~0u;
    long               stall_for     = 0;

    for (g_frame = 0; g_frame < frames; g_frame++) {
        galaga_input_update(&g_system,
                            btn_active(0, g_frame),  // coin
                            btn_active(1, g_frame),  // start1
                            btn_active(2, g_frame),  // start2
                            btn_active(3, g_frame),  // left
                            btn_active(4, g_frame),  // right
                            btn_active(5, g_frame),  // fire
                            false, false);           // rotate/mirror meta buttons
        galaga_run_frame(&g_system);
        wav_pump_frame();

        if (every > 0 && (g_frame % every) == 0) { flush_pending(); print_state("state"); }

        if (census_from >= 0 && g_frame >= census_from && g_frame <= census_to)
            sprite_census(g_frame, (int)census_code);

        if (ppm_every > 0 && (g_frame % ppm_every) == 0
            && g_frame >= ppm_from && (ppm_to == 0 || g_frame <= ppm_to)) {
            char path[1024];
            snprintf(path, sizeof(path), "%s_%05ld.ppm", ppm_prefix, g_frame);
            dump_ppm(path);
        }

        if (stall_lim > 0) {
            if (g_distinct == last_distinct && g_system.debug_checksum_pass == last_pass) {
                if (++stall_for >= stall_lim) {
                    flush_pending();
                    printf("\n*** STALLED: %ld frames with no new traced event and no "
                           "ram-test progress ***\n", stall_for);
                    print_state("stall");
                    return 3;
                }
            } else {
                stall_for = 0;
                last_distinct = g_distinct;
                last_pass = g_system.debug_checksum_pass;
            }
        }
    }

    flush_pending();
    wav_close();
    print_state("final");
    return 0;
}
