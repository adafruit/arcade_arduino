// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Host-harness driver for ArcadeMachine_BTime -- see ../README.md.
//
// Runs the REAL Burger Time machine (both actual 6502 cores, the real DECO
// CPU-7 opcode descrambler, real ROMs, real port decode and the real
// per-scanline interleaving) natively against the shared stub ArcadeHAL in
// ../host_common/.
//
// THE FLAG THAT MATTERS HERE IS `--counters`, and it exists because this
// machine's two most likely failure modes are both SILENT:
//
//  1. Burger Time has no vblank interrupt. The program finds the beam by
//     polling 0x4003 bit 7. If that read is wrong the game hangs in a wait
//     loop -- a black screen that looks exactly like a broken renderer, a
//     bad ROM load, or a dead CPU. `vblank reads` distinguishes them: a
//     nonzero count means the program is alive and looking at video timing.
//  2. The CPU-7's descrambler only fires when a write has happened since
//     the last opcode fetch AND the fetch address matches (pc & 0x104) ==
//     0x104. If the mask or the flag logic is wrong, the count is zero and
//     the machine executes plausible-looking wrong code. Zero is also what
//     you would see if the game genuinely never hit that case, so the
//     number is the only way to tell "not implemented" from "not reached".
//
// Counting the thing you actually care about, rather than inferring it from
// a rendered frame, is DEVNOTES.md problem #32.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#include "btime_machine.h"
#include "btime_video.h"
#include "btime_assets.h"
#include "btime_input.h"
#include "btime_ports.h"
#include "btime_audio.h"
#include "arcade_hal_video.h"
#include "m6502.h"

extern "C" void host_storage_set_rom_dir(const char *dir);

// --ay-trace: print every AY register write, decoded. The register names
// are AY-3-8910's own (ay8910.h's enum); the channel a write belongs to is
// what decides whether it goes through this board's band-pass, since only
// AY2 channel A does.
static bool g_ay_trace = false;
static bool g_latch_trace_on = false;
static const char *ay_reg_name(uint8_t r) {
    static const char *n[16] = {
        "A_fine", "A_coarse", "B_fine", "B_coarse", "C_fine", "C_coarse",
        "noise_period", "enable", "A_vol", "B_vol", "C_vol",
        "env_fine", "env_coarse", "env_shape", "portA", "portB"};
    return n[r & 15];
}
static void ay_trace(uint8_t chip, uint8_t reg, uint8_t val) {
    if (!g_ay_trace) return;
    printf("    AY%u  r%-2u %-13s = %02X", (unsigned)(chip + 1), (unsigned)reg,
           ay_reg_name(reg), val);
    if (reg == 7) {
        printf("   tone A%s B%s C%s | noise A%s B%s C%s",
               (val & 1) ? "off" : "ON ", (val & 2) ? "off" : "ON ",
               (val & 4) ? "off" : "ON ", (val & 8) ? "off" : "ON ",
               (val & 16) ? "off" : "ON ", (val & 32) ? "off" : "ON ");
    } else if (reg >= 8 && reg <= 10) {
        printf("   %s", (val & 0x10) ? "envelope-driven" : "fixed level");
    }
    printf("\n");
}
// Drives the machine's own registered audio fill callback -- the same one
// the board's audio ISR calls on device. See tools/host_common/hal_host.cpp.
extern "C" void host_audio_fill(int32_t *out, int count);

static btime_system g_system;
static long g_frame;

// Declared here rather than beside the other trace helper because it needs
// g_frame: a command's TIMING is the whole point of logging it.
static void latch_trace(uint8_t cmd) {
    if (g_latch_trace_on)
        printf("[frame %ld] main CPU -> sound board: 0x%02X\n", g_frame, (unsigned)cmd);
}

// FNV-1a over the emulated machine, for A/B comparisons that must not
// change emulation (see BTIME_PORT_PLAN.md's note on why byte-identical PPM
// comparison is the wrong instrument for an interleave change).
static uint64_t digest_state(void) {
    uint64_t h = 1469598103934665603ULL;
    const uint8_t *p; size_t n;
    #define FNV(ptr, len) do { p = (const uint8_t *)(ptr); n = (len); \
        for (size_t _i = 0; _i < n; _i++) { h ^= p[_i]; h *= 1099511628211ULL; } } while (0)
    FNV(&g_system.cpu.pc, sizeof(g_system.cpu.pc));
    FNV(&g_system.cpu.sp, sizeof(g_system.cpu.sp));
    FNV(&g_system.cpu.a, sizeof(g_system.cpu.a));
    FNV(&g_system.cpu.x, sizeof(g_system.cpu.x));
    FNV(&g_system.cpu.y, sizeof(g_system.cpu.y));
    FNV(&g_system.audiocpu.pc, sizeof(g_system.audiocpu.pc));
    FNV(g_system.ram, sizeof(g_system.ram));
    FNV(g_system.videoram, sizeof(g_system.videoram));
    FNV(g_system.colorram, sizeof(g_system.colorram));
    FNV(g_system.paletteram, sizeof(g_system.paletteram));
    FNV(g_system.audio_ram, sizeof(g_system.audio_ram));
    FNV(&g_system.flip_screen, sizeof(g_system.flip_screen));
    FNV(&g_system.bnj_scroll0, sizeof(g_system.bnj_scroll0));
    FNV(&g_system.soundlatch, sizeof(g_system.soundlatch));
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
        btime_video_render_scanline(&g_system, y, row);
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
    printf("  main  pc=%04X sp=%02X a=%02X x=%02X y=%02X cyc=%" PRIu32
           " %c%c%c%c%c%c\n",
           g_system.cpu.pc, g_system.cpu.sp, g_system.cpu.a, g_system.cpu.x,
           g_system.cpu.y, g_system.cpu.cyc,
           g_system.cpu.nf ? 'N' : '.', g_system.cpu.vf ? 'V' : '.',
           g_system.cpu.df ? 'D' : '.', g_system.cpu.idf ? 'I' : '.',
           g_system.cpu.zf ? 'Z' : '.', g_system.cpu.cf ? 'C' : '.');
    printf("  sound pc=%04X sp=%02X a=%02X cyc=%" PRIu32
           "  latch=%02X irq=%d nmi_en=%d\n",
           g_system.audiocpu.pc, g_system.audiocpu.sp, g_system.audiocpu.a,
           g_system.audiocpu.cyc, g_system.soundlatch,
           (int)g_system.sound_irq, (int)g_system.audio_nmi_en);
    printf("  flip=%d bnj_scroll0=%02X (bg %s) rotation=%u mirror=%d stretch=%d\n",
           (int)g_system.flip_screen, g_system.bnj_scroll0,
           (g_system.bnj_scroll0 & 0x10) ? "ON" : "off",
           (unsigned)g_system.rotation, (int)g_system.mirror_x,
           (int)btime_video_get_aspect_stretch());
    printf("  palette RAM:");
    for (int i = 0; i < 16; i++) printf(" %02X", g_system.paletteram[i]);
    printf("\n  digest=%016" PRIX64 "\n", digest_state());
}

static void print_costs(void) {
    uint32_t cpu = 0, render = 0, audio = 0;
    btime_debug_take_costs(&cpu, &render, &audio);
    const uint32_t tot = cpu + render + audio;
    printf("--- frame cost breakdown, mean over 60 frames (frame %ld) ---\n", g_frame);
    printf("  CPUs   : %6" PRIu32 "us  %5.1f%%\n", cpu,    tot ? 100.0*cpu/tot : 0.0);
    printf("  render : %6" PRIu32 "us  %5.1f%%\n", render, tot ? 100.0*render/tot : 0.0);
    printf("  audio  : %6" PRIu32 "us  %5.1f%%\n", audio,  tot ? 100.0*audio/tot : 0.0);
    printf("  total  : %6" PRIu32 "us   (host machine, NOT the device)\n", tot);
}

static void print_counters(void) {
    btime_counters c;
    memset(&c, 0, sizeof(c));
    btime_debug_take_counters(&g_system, &c);
    printf("--- counters (frame %ld, since last report) ---\n", g_frame);
    printf("  vblank-bit reads (0x4003) : %-10" PRIu32 " %s\n", c.vblank_reads,
           c.vblank_reads ? "" : "<-- ZERO: the program is not polling video timing");
    printf("  CPU-7 opcode descrambles  : %-10" PRIu32 " %s\n", c.opcode_swaps,
           c.opcode_swaps ? "" : "<-- ZERO: mask wrong, or genuinely never reached");
    printf("  swapped-mirror VRAM reads : %" PRIu32 "\n", c.mirror_reads);
    printf("  SYSTEM port reads (0x4002): %" PRIu32 "\n", c.system_reads);
    printf("  coin IRQs delivered       : %-10" PRIu32 " (in %" PRIu32
           " unmasked scanlines)\n", c.main_irqs, c.main_irq_windows);
    printf("  sound commands sent       : %-10" PRIu32 " (main CPU -> latch)\n",
           c.latch_writes);
    printf("  sound commands collected  : %-10" PRIu32 " %s\n", c.latch_reads,
           (c.latch_reads < c.latch_writes)
               ? "<-- FEWER THAN SENT: commands are being lost" : "");
    printf("  sound CPU IRQs / NMIs     : %" PRIu32 " / %" PRIu32 "\n",
           c.sound_irqs, c.sound_nmis);
    printf("  AY register writes        : %" PRIu32 "\n", c.ay_reg_writes);
    printf("  illegal opcodes (running) : %-10" PRIu32 " %s\n", c.illegal_ops,
           c.illegal_ops ? "<-- NONZERO: this ROM should not need any" : "");
}

// --- WAV capture ---------------------------------------------------------
//
// Writes a 16-bit mono WAV of exactly what the board would play, by pumping
// the machine's own fill callback once per frame. Needed here for two
// reasons: to confirm the AY synthesis makes sound at all, and to MEASURE
// the peak level, since this port's final output gain is set against a
// measurement rather than derived from MAME's netlist units.
static FILE *g_wav;
static uint32_t g_wav_samples;

static void wav_open(const char *path) {
    g_wav = fopen(path, "wb");
    if (!g_wav) { fprintf(stderr, "cannot write %s\n", path); return; }
    uint8_t hdr[44];
    memset(hdr, 0, sizeof(hdr));
    fwrite(hdr, 1, sizeof(hdr), g_wav); // patched in wav_close()
    g_wav_samples = 0;
}

static void wav_pump(void) {
    if (!g_wav) return;
    static int32_t buf[2048];
    // Drain exactly one frame's worth of REAL time, carrying the fraction.
    // The board's ISR consumes at 22050Hz of real time while frames arrive
    // at the DVI rate, so this uses 60Hz (the display), NOT the emulated
    // machine's 57.4449Hz -- draining the wrong one makes the captured WAV
    // play back at the wrong speed, which is exactly the sort of thing that
    // gets mistaken for the emulation being wrong.
    static double accum = 0.0;
    accum += (double)BTIME_AUDIO_SAMPLE_RATE / 60.0;
    int n = (int)accum;
    accum -= n;
    if (n > (int)(sizeof(buf) / sizeof(buf[0]))) n = (int)(sizeof(buf) / sizeof(buf[0]));
    host_audio_fill(buf, n);
    for (int i = 0; i < n; i++) {
        int16_t s = (int16_t)(buf[i] >> 16); // both channels carry the same mono mix
        fputc(s & 0xff, g_wav);
        fputc((s >> 8) & 0xff, g_wav);
    }
    g_wav_samples += (uint32_t)n;
}

static void put32(FILE *f, uint32_t v) { fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f); fputc((v >> 16) & 0xff, f); fputc((v >> 24) & 0xff, f); }
static void put16(FILE *f, uint16_t v) { fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f); }

static void wav_close(void) {
    if (!g_wav) return;
    uint32_t data_bytes = g_wav_samples * 2;
    fseek(g_wav, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, g_wav); put32(g_wav, 36 + data_bytes);
    fwrite("WAVEfmt ", 1, 8, g_wav); put32(g_wav, 16);
    put16(g_wav, 1); put16(g_wav, 1);
    put32(g_wav, BTIME_AUDIO_SAMPLE_RATE);
    put32(g_wav, BTIME_AUDIO_SAMPLE_RATE * 2);
    put16(g_wav, 2); put16(g_wav, 16);
    fwrite("data", 1, 4, g_wav); put32(g_wav, data_bytes);
    fclose(g_wav);
    printf("[wrote %u audio samples (%.2fs)]\n", g_wav_samples,
           (double)g_wav_samples / BTIME_AUDIO_SAMPLE_RATE);
    g_wav = NULL;
}

static void report_audio(void) {
    uint32_t under = 0, over = 0, queued = 0;
    int32_t peak = 0;
    btime_audio_debug_take_stats(&under, &over, &queued, &peak);
    printf("--- sound (frame %ld) ---\n", g_frame);
    printf("  ring queued: %" PRIu32 "   underruns: %" PRIu32
           "   overruns: %" PRIu32 "\n", queued, under, over);
    printf("  peak sample since last report: %" PRId32 " (%.1f%% of full scale)%s\n",
           peak, 100.0 * peak / 32767.0,
           peak >= 32767 ? "  <-- CLIPPING" : "");
}

// A held button, expressed as a frame window. Six frames is comfortably
// longer than any once-per-frame poll needs and shorter than a human press.
static long g_hold_frames = 6;
struct press { long at = -1; bool active(long f) const {
    return at >= 0 && f >= at && f < at + g_hold_frames; } };

// The 8 sprites, decoded the same way btime_video.cpp's draw_sprites_row()
// decodes them: attributes interleaved into video RAM at stride 0x20, both
// coordinates subtractive.
//
// This exists for the same reason dkong_host has --dma. A machine whose
// sprites are wrong does not crash and does not go blank -- it shows a
// perfectly good playfield with nothing moving on it, which reads as "the
// renderer works, something else is broken" and sends you to the wrong
// file. Printing enable/code/x/y answers "is there anything to draw, and is
// it moving" directly.
static void print_sprites(void) {
    printf("--- sprites (frame %ld) ---\n", g_frame);
    for (int i = 0; i < 8; i++) {
        const uint16_t offs = (uint16_t)(i * 4 * 0x20);
        const uint8_t attr = g_system.videoram[offs];
        const uint8_t code = g_system.videoram[offs + 0x20];
        const int y = 240 - g_system.videoram[offs + 0x40] - 1;
        const int x = 240 - g_system.videoram[offs + 0x60];
        printf("  %d: %s code=%02X x=%4d y=%4d%s%s\n", i,
               (attr & 1) ? "ON " : "off", code, x, y,
               (attr & 4) ? " flipx" : "", (attr & 2) ? " flipy" : "");
    }
}

int main(int argc, char **argv) {
    long frames = 600;
    const char *rom_dir = ".";
    const char *ppm_path = NULL;
    long ppm_at = -1;
    bool want_state = false, want_counters = false, want_costs = false;
    long counters_every = 0;
    long digest_every = 0;
    int rotation = -1;
    bool stretch = false;
    long sprites_every = 0;
    bool want_sprites = false;
    press coin, start1, up, down, left, right, pepper;
    const char *wav_path = NULL;
    long sweep_first = -1, sweep_last = -1, sweep_hold = 60, sweep_start = 300;
    // --sound-at FRAME CMD, repeatable: inject specific commands at specific
    // frames. Needed to reproduce an effect PLAYING OVER the music, which is
    // how it is actually heard and how the reference recording was made --
    // a sweep can only ever audition one thing at a time.
    long at_frame[8]; long at_cmd[8]; int at_n = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom_dir = argv[++i];
        else if (!strcmp(argv[i], "--ppm") && i + 1 < argc) ppm_path = argv[++i];
        else if (!strcmp(argv[i], "--ppm-at") && i + 1 < argc) ppm_at = atol(argv[++i]);
        else if (!strcmp(argv[i], "--state")) want_state = true;
        else if (!strcmp(argv[i], "--counters")) want_counters = true;
        else if (!strcmp(argv[i], "--counters-every") && i + 1 < argc) counters_every = atol(argv[++i]);
        else if (!strcmp(argv[i], "--digest-every") && i + 1 < argc) digest_every = atol(argv[++i]);
        else if (!strcmp(argv[i], "--rotation") && i + 1 < argc) rotation = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stretch")) stretch = true;
        else if (!strcmp(argv[i], "--coin-at") && i + 1 < argc) coin.at = atol(argv[++i]);
        else if (!strcmp(argv[i], "--start-at") && i + 1 < argc) start1.at = atol(argv[++i]);
        else if (!strcmp(argv[i], "--up-at") && i + 1 < argc) up.at = atol(argv[++i]);
        else if (!strcmp(argv[i], "--down-at") && i + 1 < argc) down.at = atol(argv[++i]);
        else if (!strcmp(argv[i], "--left-at") && i + 1 < argc) left.at = atol(argv[++i]);
        else if (!strcmp(argv[i], "--right-at") && i + 1 < argc) right.at = atol(argv[++i]);
        else if (!strcmp(argv[i], "--pepper-at") && i + 1 < argc) pepper.at = atol(argv[++i]);
        else if (!strcmp(argv[i], "--hold-frames") && i + 1 < argc) g_hold_frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav_path = argv[++i];
        else if (!strcmp(argv[i], "--ay-trace")) g_ay_trace = true;
        else if (!strcmp(argv[i], "--latch-trace")) g_latch_trace_on = true;
        else if (!strcmp(argv[i], "--sound-at") && i + 2 < argc && at_n < 8) {
            at_frame[at_n] = atol(argv[++i]);
            at_cmd[at_n]   = strtol(argv[++i], NULL, 0);
            at_n++;
        }
        else if (!strcmp(argv[i], "--sound-sweep") && i + 2 < argc) {
            sweep_first = atol(argv[++i]);
            sweep_last  = atol(argv[++i]);
        }
        else if (!strcmp(argv[i], "--sweep-hold") && i + 1 < argc) sweep_hold = atol(argv[++i]);
        else if (!strcmp(argv[i], "--sweep-start") && i + 1 < argc) sweep_start = atol(argv[++i]);
        else if (!strcmp(argv[i], "--costs")) want_costs = true;
        else if (!strcmp(argv[i], "--sprites")) want_sprites = true;
        else if (!strcmp(argv[i], "--sprites-every") && i + 1 < argc) sprites_every = atol(argv[++i]);
        else {
            fprintf(stderr,
                "usage: %s [--frames N] [--rom DIR] [--rotation 0..3] [--stretch]\n"
                "          [--ppm FILE] [--ppm-at N] [--state] [--counters]\n"
                "          [--counters-every N] [--digest-every N]\n"
                "          [--sprites] [--sprites-every N]\n"
                "          [--coin-at N] [--start-at N] [--hold-frames N]\n"
                "          [--up-at N] [--down-at N] [--left-at N] [--right-at N]\n"
                "          [--pepper-at N] [--wav FILE]\n"
                "          [--sound-sweep FIRST LAST] [--sweep-hold N] [--sweep-start N]\n"
                "          [--sound-at FRAME CMD]... [--ay-trace]\n",
                argv[0]);
            return 2;
        }
    }

    host_storage_set_rom_dir(rom_dir);

    btime_init(&g_system);
    if (rotation >= 0) g_system.rotation = (uint8_t)(rotation & 3);
    btime_video_set_aspect_stretch(stretch);

    uint16_t err = 0;
    if (!btime_load_assets(&g_system, &err)) {
        fprintf(stderr, "asset load failed (error colour %04X); missing: %s\n",
                err, btime_debug_missing_files());
        return 1;
    }
    if (btime_debug_missing_files()[0])
        printf("[note] optional files missing: %s\n", btime_debug_missing_files());

    if (wav_path) wav_open(wav_path);
    if (g_ay_trace) btime_audio_set_reg_trace(&ay_trace);
    if (g_latch_trace_on) btime_debug_set_latch_trace(&latch_trace);

    print_state("after reset");

    for (g_frame = 0; g_frame < frames; g_frame++) {
        btime_input_update(&g_system,
                           coin.active(g_frame), start1.active(g_frame), false,
                           up.active(g_frame), down.active(g_frame),
                           left.active(g_frame), right.active(g_frame),
                           pepper.active(g_frame),
                           false, false);              // rotate/mirror
        for (int k = 0; k < at_n; k++) {
            if (g_frame == at_frame[k]) {
                btime_debug_inject_sound_command(&g_system, (uint8_t)at_cmd[k]);
                printf("[frame %ld] sound command 0x%02lX  (wav t=%.2fs)\n",
                       g_frame, at_cmd[k], (double)g_frame / 60.0);
            }
        }

        // Sound-command sweep: inject one command per slot so each effect
        // lands in its own stretch of the captured WAV, isolated.
        if (sweep_first >= 0 && g_frame >= sweep_start) {
            const long slot = (g_frame - sweep_start) / sweep_hold;
            const long cmd  = sweep_first + slot;
            if (cmd <= sweep_last && (g_frame - sweep_start) % sweep_hold == 0) {
                btime_debug_inject_sound_command(&g_system, (uint8_t)cmd);
                printf("[frame %ld] slot %ld: sound command 0x%02lX  (wav t=%.2fs)\n",
                       g_frame, slot, cmd,
                       (double)g_frame / 60.0);
            }
        }

        btime_run_frame(&g_system);
        wav_pump();

        if (g_frame == ppm_at && ppm_path) dump_ppm(ppm_path);
        if (counters_every && (g_frame % counters_every) == counters_every - 1)
            print_counters();
        if (sprites_every && (g_frame % sprites_every) == sprites_every - 1)
            print_sprites();
        if (digest_every && (g_frame % digest_every) == digest_every - 1)
            printf("frame %ld digest=%016" PRIX64 "\n", g_frame, digest_state());
    }
    g_frame = frames;

    if (want_state) print_state("final");
    if (want_sprites) print_sprites();
    if (want_counters) print_counters();
    if (want_costs) print_costs();
    if (wav_path) { report_audio(); wav_close(); }
    if (ppm_path && ppm_at < 0) dump_ppm(ppm_path);
    return 0;
}
