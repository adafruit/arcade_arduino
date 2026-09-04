// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Lunar Rescue machine lifecycle -- structurally identical to
// ArcadeMachine_Invaders' invaders_machine.cpp (same CPU clock, same
// two-interrupts-per-frame scheme: both games are on the same "8080bw"
// board family, confirmed via mw8080bw_root()'s shared machine_config in
// MAME's midw8080/mw8080bw.cpp -- the 1.9968MHz CPU clock and the RST1
// mid-screen / RST2 vblank interrupt pair are hardware facts of that shared
// board, not something Space Invaders' ROM does specially).
#include <string.h>
#include <stdint.h>
#include "lrescue_machine.h"
#include "lrescue_ports.h"
#include "lrescue_video.h"
#include "lrescue_audio.h"
#include "lrescue_assets.h"
#include "arcade_hal_video.h"
#include "arcade_video_geom.h"
#include "arcade_hal_audio.h"
#include "arcade_hal_storage.h"
#include "arcade_hal_input.h"
#include <Arduino.h> // DEBUG: micros() for the frame-timing measurement below

// 59.541985 is the ORIGINAL arcade board's exact vertical refresh rate (same
// mw8080bw_root() machine_config as Space Invaders -- see the file header
// comment above) -- but this emulator's own frame pacing comes entirely
// from PicoDVI's blocking scanline queue on THIS hardware (see CLAUDE.md),
// not from that original CRT's timing, and measurement on a real Fruit Jam
// found the two don't match: system->total_cycles (which advances by
// CYCLES_PER_FRAME once per real frame, and is what lrescue_ports.cpp
// timestamps speaker events against) was found running ~0.83% faster than
// the audio ISR's own real-time-paced clock (lrescue_audio.cpp's
// g_target_cycle, driven by the actual audio sample rate) -- consistently,
// across two independent ~33-66 second sessions, both endpoint and midpoint
// slope estimates agreeing to within measurement noise. That 0.83% is a
// real, non-closing rate mismatch, not drift or jitter: it means real DVI
// frames on this hardware arrive at roughly 60.02-60.04Hz, not 59.541985Hz.
// 60.0368 below is that empirical measurement (60.036778 from the widest-
// span two-point calibration; a 60.015915 midpoint cross-check agreed
// closely enough to confirm this is a fixed constant-rate mismatch, not
// something curvier). This constant governs BOTH overall game speed
// (CYCLES_PER_FRAME -- how many i8080 instructions execute per real frame)
// and, transitively, how accurately total_cycles tracks real elapsed time
// for audio-event timestamping -- so this single correction fixes a
// (previously unnoticed) ~0.83% gameplay-speed inaccuracy versus the
// original hardware AND the audio-desync bug that motivated measuring this
// in the first place (see lrescue_ports.cpp's speaker-event call for why
// the desync fix belongs here and not in which clock domain that call
// uses). If PicoDVI's video timing parameters ever change, re-measure via
// lrescue_run_frame()'s per-second gap= print -- after this correction it
// should stay flat/bounded instead of growing linearly with play time.
#define FRAMERATE          60.0368
#define CYCLES_PER_FRAME   (LRESCUE_CPU_HZ / FRAMERATE) // ~33,260 cycles/frame

void lrescue_init(arcade_system *system) {
    memset(&system->state, 0, sizeof(system->state));

    // Build the canvas mapping for this raster (arcade_video_geom.h). Must
    // happen before the first frame -- the renderer reads av_tate/av_yoko on
    // every scanline and they are all zeroes until this runs. LONG axis
    // first (GAME_WIDTH), then SHORT (GAME_HEIGHT).
    av_geom_init(LRESCUE_GAME_WIDTH, LRESCUE_GAME_HEIGHT);


    // IMPORTANT: leave state.mirror_2000_at_4000 false (its memset default).
    // Space Invaders' PCB incompletely decodes its address bus and aliases
    // RAM at 0x2000-0x3fff onto 0x4000-0x5fff; Lunar Rescue's PCB maps real
    // ROM chips (lrescue.5/lrescue.6) into that exact range instead (see
    // lrescue_assets.h) -- setting this true here would make every read
    // from that ROM silently return stale RAM bytes. See i8080.h's
    // Cpu_state.mirror_2000_at_4000 doc comment.

    system->left   = 0;
    system->right  = 0;
    system->shot   = 0;
    system->start1 = 0;
    system->start2 = 0;
    system->coin   = 0;
    system->tilt   = 0;

    system->ext_shift_offset = 0;
    system->ext_shift_data   = 0;
    system->screen_red    = false;
    system->flip_screen    = 0;
    system->rotation = 1;   // default: 90 deg CCW tate mode
    system->mirror_x = false;

    hal_video_init();
    lrescue_ports_bind(system);
}

bool lrescue_load_assets(arcade_system *system, uint16_t *out_error_color) {
    lrescue_rom_load_status_t rom_status = lrescue_load_rom(system);
    if (rom_status == LRESCUE_ROM_LOAD_NO_STORAGE) {
        *out_error_color = LRESCUE_COLOR_ERROR_NO_CARD;
        return false;
    }
    if (rom_status == LRESCUE_ROM_LOAD_NO_ROM_FILES) {
        *out_error_color = LRESCUE_COLOR_ERROR_NO_ASSETS;
        return false;
    }

    hal_audio_init(LRESCUE_AUDIO_SAMPLE_RATE);
    int samples_loaded = lrescue_audio_load_samples();
    hal_storage_unmount();
    if (samples_loaded == 0) {
        *out_error_color = LRESCUE_COLOR_ERROR_NO_ASSETS;
        return false;
    }

    hal_input_init();
    return true;
}

// One emulated instruction plus the two per-frame interrupt checks --
// lifted VERBATIM out of lrescue_run_frame()'s old single loop when that
// loop was split to interleave scanline output (see the comment there).
// Factored into one place precisely so the two call sites cannot drift
// apart from each other, or from the original semantics: `cyc` is an
// absolute running cycle count and both thresholds are absolute, so the
// mid-frame and vblank interrupts fire at exactly the same emulated
// instants as before the split.
// Worst (smallest) producer lead seen since the last report -- see the
// sampling site in lrescue_run_frame().
static int64_t g_min_lead = INT64_MAX;

static inline void step_cpu(arcade_system *system, int *cyc, int *int_state) {
    int delta = exec_opcode(&system->state);
    *cyc += delta;
    system->total_cycles += (uint64_t)delta; // see lrescue_machine.h's doc comment on this field
    if (*cyc >= (int)(CYCLES_PER_FRAME / 2) && *int_state == 0) {
        *int_state = 1;
        int idelta = interrupt(&system->state, 1);
        *cyc += idelta;
        system->total_cycles += (uint64_t)idelta;
    }
    if (*cyc >= (int)CYCLES_PER_FRAME && *int_state == 1) {
        *int_state = 2;
        int idelta = interrupt(&system->state, 2);
        *cyc += idelta;
        system->total_cycles += (uint64_t)idelta;
    }
}

void lrescue_run_frame(arcade_system *system) {
    // DEBUG (kept minimal -- see below): earlier versions of this function
    // printed a new Serial.printf() line every time either half of a frame
    // (CPU-emulation loop, render pass) set a new worst-case time, plus
    // lrescue_ports.cpp printed on every port3/port5 change. That
    // instrumentation did its job (it's what established CPU-loop and
    // render/draw time weren't the cause of the red-scanline/crumbly-audio
    // investigation), but a print call itself takes real microseconds --
    // and having *served* that purpose is exactly why it's now worth
    // removing rather than leaving in: this game's real per-frame time
    // budget (~16.66ms, set by PicoDVI's actual achieved frame rate, not
    // this file's FRAMERATE assumption -- see that constant's doc comment)
    // has since been shown to have a worst-case margin as thin as ~1.1ms,
    // and unconditional per-event debug printing during exactly bonus1's
    // rapid port-write activity was never itself ruled out as eating into
    // that margin. Reporting only once per second now, below.
    uint32_t t0 = micros();

    // `cyc` persists across frames -- any cycles run past this frame's
    // budget are carried forward and subtracted from the next frame's
    // budget, matching ArcadeMachine_Invaders' timing exactly (same shared
    // board clock). Its arithmetic, and both interrupts' absolute cycle
    // thresholds, are UNCHANGED by the interleaving below: `cyc` is still
    // an absolute running count, so `CYCLES_PER_FRAME / 2` and
    // `CYCLES_PER_FRAME` still fire at exactly the same emulated instants
    // they always did. That matters -- this counter is what keeps the
    // emulated clock aligned with real time for the audio path, and
    // DEVNOTES.md problem #15 is a full account of how easy that clock
    // domain is to get wrong here.
    static int cyc = 0;
    int int_state = 0;

    // INTERLEAVED CPU + scanline output. This function used to run the
    // whole frame's emulation (~1.8ms) in one uninterrupted loop and only
    // then call lrescue_draw_frame(), i.e. with ZERO
    // hal_video_acquire_scanline()/hal_video_submit_scanline() calls until
    // every cycle was done -- the same bug DEVNOTES.md problem #20 fixed
    // for Pac-Man, never back-applied to the two i8080 games. During that
    // burst Core 1 could only coast on the 8-buffer scanline queue (~555us,
    // a hard ceiling in the vendored libdvi) plus the vertical blanking
    // interval (~1.4ms), i.e. ~2ms of cover for a 1.8ms burst -- about
    // 200us of margin, which a single 232us audio ISR exceeds outright.
    // That is the whole explanation for this game's long-standing red
    // lines under heavy audio (DEVNOTES.md problem #34, which closes out
    // the investigation in problem #16).
    //
    // Unlike Pac-Man, no second sequential path is needed for any rotation:
    // this renderer reads live VRAM on demand for every rotation and keeps
    // no frame cache, so every rotation can interleave.
    //
    // Consequence, and it is the intended one: a scanline now renders from
    // whatever VRAM state exists at that point in the frame's execution
    // rather than the frame's final state. That is MORE faithful, not less
    // -- real scanline-order CRT hardware behaves exactly this way -- and
    // the same change on Pac-Man produced no visible tearing.
    const int cyc_start = cyc;
    const int cyc_total = (int)CYCLES_PER_FRAME - cyc_start;
    uint32_t render_sum = 0, block_sum = 0;

    for (uint32_t i = 0; i < HAL_VIDEO_HEIGHT; i++) {
        // Exact proportional target (not repeated addition) so the final
        // slice lands precisely on CYCLES_PER_FRAME however that divides by
        // the scanline count -- same shape as pacman_machine.cpp's
        // run_frame_interleaved().
        // Deliberately 32-bit: cyc_total is ~33000 and the scanline count
        // 240, so the product peaks around 8e6 and cannot overflow int32.
        // An int64 divide here would be a flash-resident library call run
        // 240 times per frame -- precisely the cost DEVNOTES.md problem #17
        // warns about.
        int target = cyc_start +
            (cyc_total * (int)(i + 1)) / (int)HAL_VIDEO_HEIGHT;
        while (int_state != 2 && cyc < target) {
            step_cpu(system, &cyc, &int_state);
        }

        // Minimum producer lead, sampled every scanline rather than once
        // per second at frame end. The once-per-second sample cannot see
        // the worst instant, and the worst instant is what matters: the
        // audio ISR fills a whole buffer at once, advancing target_cycle
        // by BUFFER_SAMPLES * cycles-per-sample (~11600 cycles at 128
        // samples) in one go. If the lead ever drops below that span, the
        // tail of that buffer resolves speaker levels against emulated
        // time the CPU has not reached yet -- the level sticks and then
        // catches up abruptly, which is what the crunchy arpeggio is.
        {
            int64_t lead = (int64_t)system->total_cycles -
                           (int64_t)lrescue_audio_debug_target_cycle();
            if (lead < g_min_lead) g_min_lead = lead;
        }

        uint32_t a = micros();
        uint16_t *buf = hal_video_acquire_scanline();
        uint32_t b = micros();
        lrescue_video_render_scanline(i, buf, system);
        uint32_t c = micros();
        hal_video_submit_scanline(buf);
        uint32_t d = micros();
        block_sum  += (b - a) + (d - c);
        render_sum += (c - b);
    }

    // Integer rounding above can leave the last few cycles (and therefore
    // the vblank interrupt) unrun. Finish them.
    while (int_state != 2) {
        step_cpu(system, &cyc, &int_state);
    }

    cyc = (int)CYCLES_PER_FRAME - cyc;

    // DEBUG: once-per-second frame-budget + producer/consumer clock-
    // agreement report. worst_frame_us (this function's total time,
    // including the CPU-emulation loop above) was found sitting right at
    // ~16660-16830us basically every single second sampled, not just around
    // bonus1 -- which turned out to be expected, not a red flag on its own:
    // hal_video_acquire_scanline()/hal_video_submit_scanline() deliberately
    // BLOCK to pace Core 0 against Core 1's real DVI rate, so a measurement
    // spanning those calls reads close to the true frame period whether
    // Core 0 is comfortably idle-waiting or genuinely falling behind --
    // those two situations look identical in this one number (see
    // lrescue_draw_frame()'s doc comment). render_us/block_us split that
    // apart: render_us is pure CPU-bound render compute (should stay small
    // and roughly content-independent); block_us is time actually spent
    // waiting on Core 1. If render_us climbs specifically around a red-line
    // report, Core 0's own compute is the bottleneck (e.g. VRAM content
    // with many more lit pixels than usual costing more color-PROM lookups)
    // -- if only block_us climbs, Core 1 (or the shared DMA/bus) is what's
    // actually falling behind, not this loop. gap= is what caught
    // FRAMERATE's ~0.83% mismatch against this hardware's real achieved DVI
    // rate (see that constant's doc comment) -- should stay flat/small now;
    // if it starts growing again (e.g. after a PicoDVI timing change),
    // that's the signal to re-measure it.
    static uint32_t worst_frame_us = 0, worst_render_us = 0, worst_block_us = 0, report_frame_count = 0;
    uint32_t frame_us = micros() - t0;
    if (frame_us > worst_frame_us) worst_frame_us = frame_us;
    // Accumulated by the interleaved loop above, rather than read back from
    // lrescue_draw_frame() (which this function no longer calls).
    uint32_t render_us = render_sum, block_us = block_sum;
    if (render_us > worst_render_us) worst_render_us = render_us;
    if (block_us  > worst_block_us)  worst_block_us  = block_us;
    if (++report_frame_count >= 60) {
        report_frame_count = 0;
        uint64_t target = lrescue_audio_debug_target_cycle();
        int64_t gap = (int64_t)system->total_cycles - (int64_t)target;

        // DEBUG: the audio ISR's own cost + peak concurrently-active
        // channel count over the same ~1s window, per the leading
        // hypothesis for the remaining red lines (two players shooting at
        // once, several bonus1-adjacent sounds -- more active channels
        // means this ISR runs longer per invocation, stealing more time
        // from whatever render_us/block_us above was measuring at the
        // moment it fired, which those two numbers alone can't see). isr_us
        // is TOTAL time spent inside this ISR across the whole window, so
        // isr_us/isr_calls is this window's AVERAGE per-invocation cost --
        // max_isr_call_us is the single WORST invocation instead, which an
        // average can dilute away: if that spikes around a red-line report
        // while the average barely moves, it's a rare single-call outlier
        // (e.g. a channel hitting a loop-restart edge case), not a sustained
        // per-channel cost increase -- a materially different cause than
        // the "more channels -> proportionally more average cost" pattern
        // already confirmed. Compare both against max_channels.
        uint32_t isr_us = 0, max_isr_call_us = 0, max_channels = 0, isr_calls = 0;
        lrescue_audio_debug_isr_stats(&isr_us, &max_isr_call_us, &max_channels, &isr_calls);

        // Speaker-event producer/consumer health. Added when interleaving
        // the CPU with the scanline pump (problem #34) fixed the red lines
        // but made the bonus arpeggio sound crunchy: the interleave changed
        // WHEN within a frame speaker events are produced relative to when
        // the audio ISR consumes them, and this queue's behaviour is
        // sensitive to exactly that. `dropped` non-zero means the queue
        // overflowed; `drain_hits` non-zero means the consumer could not
        // keep up within one call's DRAIN_LIMIT; `peak` is how deep the
        // queue ever got. All three were invisible before this line.
        uint32_t sp_pushed = 0, sp_dropped = 0, sp_peak = 0, sp_drain_hits = 0;
        lrescue_audio_speaker_debug_stats(&sp_pushed, &sp_dropped, &sp_peak, &sp_drain_hits);

        Serial.printf("lrescue_run_frame: worst frame %lu us (render %lu / block %lu, budget ~16660us) | "
                      "isr: %lu us total / %lu calls (avg %lu / worst-single %lu us) / max %lu active channels | "
                      "total_cycles=%llu target_cycle=%llu gap=%+lld cyc (%+.1fms) | "
                      "spk: pushed=%lu dropped=%lu peak=%lu drain_hits=%lu min_lead=%s%lld cyc (%.1fms)\n",
                      (unsigned long)worst_frame_us, (unsigned long)worst_render_us, (unsigned long)worst_block_us,
                      (unsigned long)isr_us, (unsigned long)isr_calls,
                      (unsigned long)(isr_calls ? isr_us / isr_calls : 0), (unsigned long)max_isr_call_us,
                      (unsigned long)max_channels,
                      (unsigned long long)system->total_cycles, (unsigned long long)target,
                      (long long)gap, (double)gap / 1996.8,
                      (unsigned long)sp_pushed, (unsigned long)sp_dropped,
                      (unsigned long)sp_peak, (unsigned long)sp_drain_hits,
                      g_min_lead == INT64_MAX ? "UNSAMPLED:" : "",
                      (long long)(g_min_lead == INT64_MAX ? 0 : g_min_lead),
                      (double)(g_min_lead == INT64_MAX ? 0 : g_min_lead) / 1996.8);
        g_min_lead = INT64_MAX;
        worst_frame_us = 0;
        worst_render_us = 0;
        worst_block_us = 0;
    }
}
