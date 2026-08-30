// Galaga machine lifecycle -- orchestrates 3x ArcadeCPU_Z80 instances +
// this machine's own port/video/asset modules, talking to hardware only
// through ArcadeHAL. Same overall shape as pacman_machine.cpp, but the
// frame-stepping logic is genuinely new: Pac-Man's single-CPU "run the
// whole frame's cycles, then interrupt once" loop does not apply to a
// 3-CPU shared-RAM machine -- see below.
#include <string.h>
#include "galaga_machine.h"
#include "galaga_ports.h"
#include "galaga_video.h"
#include "galaga_assets.h"
#include "galaga_audio.h"
#include "arcade_hal_video.h"
#include "arcade_hal_audio.h"
#include "arcade_hal_input.h"
#include "arcade_hal_storage.h"

// Z80 clock 18.432MHz/6 = 3.072MHz, same derivation as Pac-Man's -- and
// because Galaga's screen_raw (384x264 total, 288x224 visible) is
// byte-for-byte the same raster timing Pac-Man's is (verified in
// galaga_machine.h's header comment), the cycles/frame value comes out
// IDENTICAL too: 3072000 * 384 * 264 / (18432000/3) = 50688, no
// remainder -- not re-derived, just reused.
// CPU stepping loop -> SRAM, same XIP-cache rationale as ArcadeCPU_Z80's
// Z80_RAMFUNC (see z80.c).
#if defined(ARDUINO_ARCH_RP2040) || defined(PICO_ON_DEVICE)
#define GALAGA_M_RAMFUNC __attribute__((section(".time_critical.galagam")))
#else
#define GALAGA_M_RAMFUNC
#endif

#define GALAGA_CYCLES_PER_FRAME 50688UL

// Real hardware time-slices the 3 CPUs' shared bus at
// config.set_maximum_quantum(attotime::from_hz(6000)) -- i.e. every
// 3072000/6000 = 512 Z80 cycles, which is what this uses.
//
// This was temporarily lowered to 16 while chasing a 3-CPU boot deadlock,
// on the theory that finer interleaving was needed (danjulio/gcore_galagino
// interleaves every 4 instructions). It was not: the deadlock was a DIP
// switch bug (see galaga_assets.cpp's dswa comment), and the fine quantum
// never fixed anything. It does cost real time -- measured ~18% of total
// frame time in the host harness, purely in outer-loop overhead -- and on
// the Fruit Jam that overhead pushed galaga_run_frame() to ~19-20ms
// against a 16.67ms budget, so Core 0 could not keep the DVI scanline
// queue fed and the display went solid red (this board's libdvi fork's
// failure signature for a starved pipeline -- see hal_video_fruitjam.cpp).
//
// Note the effective quantum is additionally capped by run_frame_
// interleaved()'s per-scanline targets (GALAGA_CYCLES_PER_FRAME / 240 =
// ~211 cycles), so any value above ~211 behaves identically -- 512 is kept
// because it is the real, cited hardware figure.
#define GALAGA_QUANTUM_CYCLES 512UL

// Steps one CPU up to `target` elapsed cycles since `start` (wraparound-
// safe unsigned subtraction, same idiom pacman_machine.cpp's
// run_frame_sequential()/run_frame_interleaved() use and document in
// full -- see that file for the DEVNOTES.md problem #22 wraparound
// lesson, which applies here identically since z80.cyc is the same kind
// of never-reset running counter), but never more than one quantum slice
// past its current position -- this is what makes the interleave actually
// interleave instead of finishing one CPU before starting the next.
GALAGA_M_RAMFUNC static void step_cpu_slice(z80 *cpu, uint32_t start, uint32_t target) {
    uint32_t elapsed = (uint32_t)(cpu->cyc - start);
    if (elapsed >= target) return;
    uint32_t slice_end = elapsed + GALAGA_QUANTUM_CYCLES;
    if (slice_end > target) slice_end = target;
    while ((uint32_t)(cpu->cyc - start) < slice_end) {
        z80_step(cpu);
    }
}

// Round-robins all 3 CPUs in GALAGA_QUANTUM_CYCLES slices until each has
// reached `target` elapsed cycles since its own `start` snapshot.
// nmi2_mark_a/b are absolute cpu_sub2.cyc thresholds (computed once per
// frame by the caller) for sub2's twice-per-frame NMI -- see
// galaga_machine.h's nmi2_fired_a/_b field comment.
GALAGA_M_RAMFUNC static void interleave_to_target(galaga_system *sys, uint32_t start_main,
                                  uint32_t start_sub, uint32_t start_sub2,
                                  uint32_t target,
                                  uint32_t nmi2_mark_a, uint32_t nmi2_mark_b) {
    for (;;) {
        bool main_more = (uint32_t)(sys->cpu_main.cyc - start_main) < target;
        // sub/sub2 are held in RESET (not executing at all -- see
        // galaga_machine.h's sub_reset_released field comment) until main
        // CPU releases them via misclatch bit 3. While held, they simply
        // don't run, same as real hardware.
        //
        // Once released, sub/sub2 must not "rush" through more than one
        // frame's worth of cycles in a single burst on the specific frame
        // release happens mid-frame -- their frame-start `start_sub`/
        // `start_sub2` snapshot predates release, so the normal `target`
        // (cycles since FRAME start) would otherwise ask them to catch up
        // to wherever main currently is in the frame all at once, racing
        // far ahead of where real hardware (one shared oscillator) would
        // have them by the same point in main's boot sequence. Fix: reduce
        // sub/sub2's *effective* target, on the release frame only, by how
        // many main-cycles had already elapsed in this frame before
        // release happened (`elapsed_at_release`). On every later frame
        // `reset_release_main_cyc` predates `start_main`, so the signed
        // subtraction goes negative and clamps to 0 -- sub/sub2 get the
        // full normal `target` from then on, with no ongoing throttle.
        // An earlier attempt capped sub/sub2's *absolute* cyc at
        // `main.cyc - reset_release_main_cyc` for all future frames, not
        // just the release frame -- that over-corrected: once sub caught
        // up to being only a handful of cycles behind main, it had no
        // slack left to ever burst through a multi-instruction interrupt
        // handler again, confirmed via SWD as a permanent starvation (sub
        // pinned at a fixed PC indefinitely, even as main.cyc kept
        // growing). See project memory (galaga-port-research.md) for the
        // full investigation.
        uint32_t sub_target = target;
        if (sys->sub_reset_released) {
            // How far INTO this frame the reset release happened.
            //
            // This must be unsigned wraparound arithmetic, not a signed
            // difference. An earlier version computed
            // `(int32_t)(reset_release_main_cyc - start_main)` and clamped
            // negatives to 0, which is correct only while that difference
            // fits in a signed 32-bit value. `reset_release_main_cyc` is
            // fixed at boot (~3.5e7) while `start_main` grows ~50688 per
            // frame, so after roughly 2^31 cycles -- about 43,000 frames,
            // ~12 minutes of play -- the subtraction overflowed and wrapped
            // POSITIVE. The `< 0` clamp then never fired, sub_target
            // computed to 0, and sub/sub2 stopped being stepped entirely
            // while main kept running: the game froze with all three CPUs
            // parked in their idle loops and the shared handshake bytes
            // stuck. Found on hardware after ~13 minutes of attract mode.
            //
            // Unsigned is correct here without any clamp: once release
            // predates this frame's start (every frame after the release
            // frame), the subtraction underflows to a huge value which is
            // necessarily >= target, so no reduction is applied -- which is
            // exactly the desired behaviour.
            uint32_t into_frame = (uint32_t)(sys->reset_release_main_cyc - start_main);
            if (into_frame < target) sub_target = target - into_frame;
        }
        bool sub_more  = sys->sub_reset_released &&
                          (uint32_t)(sys->cpu_sub.cyc  - start_sub)  < sub_target;
        bool sub2_more = sys->sub_reset_released &&
                          (uint32_t)(sys->cpu_sub2.cyc - start_sub2) < sub_target;
        if (!main_more && !sub_more && !sub2_more) break;
        if (main_more) {
            step_cpu_slice(&sys->cpu_main, start_main, target);
            // DEBUG: see galaga_machine.h's debug_checksum_pass/_fail
            // field comment -- red-screen investigation instrumentation.
            if (sys->cpu_main.pc == 0x34C9) sys->debug_checksum_pass++;
            if (sys->cpu_main.pc == 0x34CA) sys->debug_checksum_fail++;
            // 06XX periodic main-CPU NMI -- see galaga_machine.h's
            // io06_nmi_period/io06_nmi_next field comments for the exact
            // formula/citation. Wraparound-safe signed-cast comparison,
            // same idiom the rest of this codebase uses for cyc deltas.
            if (sys->io06_nmi_period != 0 &&
                (int32_t)(sys->cpu_main.cyc - sys->io06_nmi_next) >= 0) {
                z80_gen_nmi(&sys->cpu_main);
                sys->io06_nmi_next += sys->io06_nmi_period;
            }
        }
        if (sub_more)  step_cpu_slice(&sys->cpu_sub,  start_sub,  sub_target);
        if (sub2_more) {
            step_cpu_slice(&sys->cpu_sub2, start_sub2, sub_target);
            // Sub2's twice-per-frame NMI -- see galaga_machine.h's
            // nmi2_fired_a/_b field comment for the citation.
            if (!sys->nmi2_fired_a && (int32_t)(sys->cpu_sub2.cyc - nmi2_mark_a) >= 0) {
                if (sys->nmi2_enable) z80_gen_nmi(&sys->cpu_sub2);
                sys->nmi2_fired_a = true;
            }
            if (!sys->nmi2_fired_b && (int32_t)(sys->cpu_sub2.cyc - nmi2_mark_b) >= 0) {
                if (sys->nmi2_enable) z80_gen_nmi(&sys->cpu_sub2);
                sys->nmi2_fired_b = true;
            }
        }
    }
}

// Fires the frame's once-per-frame interrupts -- verified against
// galaga()'s machine_config: main/sub get a vblank-sourced IRQ gated by
// irq1_enable/irq2_enable (misclatch Q0/Q1). Real Z80 interrupt MODE
// (IM0/1/2) is whatever each CPU's own program sets at runtime via the
// `IM` instruction -- z80_gen_int()'s data byte is only consulted in
// IM0, so passing 0 is safe regardless of mode (unlike Pac-Man's main
// CPU, which specifically runs in IM0 and needs a real vector byte -- no
// equivalent "OUT (0),A vector-setting" port write was found in Galaga's
// I/O map this session, see galaga_ports.cpp). Sub2's NMI is NOT fired
// here -- see galaga_machine.h's nmi2_fired_a/_b field comment, it's a
// twice-per-frame mid-frame pulse handled inside interleave_to_target().
static void fire_interrupts(galaga_system *sys) {
    if (sys->irq1_enable) z80_gen_int(&sys->cpu_main, 0);
    if (sys->irq2_enable) z80_gen_int(&sys->cpu_sub, 0);
}

// Landscape/180-degree rotation: same reasoning as
// pacman_machine.cpp's run_frame_sequential() -- those orientations need
// the whole frame's final VRAM state before galaga_draw_frame()'s
// frame_cache can render even one scanline, so there's no benefit to
// interleaving CPU execution with scanline submission here.
// Peak per-scanline render cost and the longest run of non-blocking scanline
// acquires -- the starvation detector described at the loop that feeds it.
#include <Arduino.h> // micros() for that detector
static uint32_t g_render_max_us = 0, g_noblock_run = 0, g_noblock_run_max = 0;

static void run_frame_sequential(galaga_system *system) {
    uint32_t start_main = system->cpu_main.cyc;
    uint32_t start_sub  = system->cpu_sub.cyc;
    uint32_t start_sub2 = system->cpu_sub2.cyc;
    system->nmi2_fired_a = false;
    system->nmi2_fired_b = false;
    galaga_video_begin_frame(system); // latch this frame's sprites (see galaga_video.h)

    interleave_to_target(system, start_main, start_sub, start_sub2, GALAGA_CYCLES_PER_FRAME,
                          start_sub2 + GALAGA_CYCLES_PER_FRAME / 4,
                          start_sub2 + 3UL * GALAGA_CYCLES_PER_FRAME / 4);
    fire_interrupts(system);
    galaga_draw_frame(system);
}

// Tate/CW rotation: interleaves the 3-CPU stepping WITH per-scanline
// rendering, evenly spread across HAL_VIDEO_SCANLINES_PER_FRAME calls --
// same DEVNOTES.md problem #19 rationale pacman_machine.cpp's
// run_frame_interleaved() documents in full (never run a whole frame's
// CPU cycles before the first hal_video_acquire_scanline() call).
GALAGA_M_RAMFUNC static void run_frame_interleaved(galaga_system *system) {
    uint32_t start_main = system->cpu_main.cyc;
    uint32_t start_sub  = system->cpu_sub.cyc;
    uint32_t start_sub2 = system->cpu_sub2.cyc;
    uint32_t step = HAL_VIDEO_HEIGHT / HAL_VIDEO_SCANLINES_PER_FRAME;
    system->nmi2_fired_a = false;
    system->nmi2_fired_b = false;
    galaga_video_begin_frame(system); // latch this frame's sprites (see galaga_video.h)
    uint32_t nmi2_mark_a = start_sub2 + GALAGA_CYCLES_PER_FRAME / 4;
    uint32_t nmi2_mark_b = start_sub2 + 3UL * GALAGA_CYCLES_PER_FRAME / 4;

    for (uint32_t i = 0; i < HAL_VIDEO_SCANLINES_PER_FRAME; i++) {
        uint32_t target_delta =
            (uint32_t)((uint64_t)GALAGA_CYCLES_PER_FRAME * (i + 1) / HAL_VIDEO_SCANLINES_PER_FRAME);
        interleave_to_target(system, start_main, start_sub, start_sub2, target_delta,
                              nmi2_mark_a, nmi2_mark_b);

        // Starvation detector. A red line means Core 1's VALID scanline
        // queue emptied. That cannot be seen directly from here, but its
        // mirror image can: when Core 0 is keeping up it runs AHEAD and
        // hal_video_acquire_scanline() blocks waiting for Core 1 to free a
        // buffer. When Core 0 falls behind, free buffers are plentiful and
        // acquire stops blocking. So a RUN of consecutive non-blocking
        // acquires is Core 0 losing ground, and a run approaching the
        // queue depth (8, a hard libdvi ceiling) means the valid queue has
        // drained and a red line is imminent.
        //
        // This exists because per-frame `work` was misleading here: it
        // peaked at 14946us of 16660 -- 90%, never over -- while red lines
        // still appeared. Frame totals cannot see a deficit that builds
        // over a band of expensive scanlines (a full enemy formation) and
        // is repaid over cheap ones.
        uint32_t acq0 = micros();
        uint16_t *buf = hal_video_acquire_scanline();
        uint32_t acq_us = micros() - acq0;
        // Skip the first 16 scanlines. Core 1 releases all 8 buffers when it
        // finishes a frame, so Core 0's opening acquires legitimately do not
        // block -- that is the pipeline refilling, not starving. The first
        // version of this counter did not exclude them and therefore read a
        // constant 12 on every frame regardless of load, which looked like a
        // permanent starvation and was nothing of the kind.
        if (i < 16u) {
            g_noblock_run = 0;
        } else if (acq_us < 2u) {
            if (++g_noblock_run > g_noblock_run_max) g_noblock_run_max = g_noblock_run;
        } else {
            g_noblock_run = 0;
        }
        uint32_t r0 = micros();
        galaga_video_render_scanline(system, i * step, buf);
        uint32_t r_us = micros() - r0;
        if (r_us > g_render_max_us) g_render_max_us = r_us;
        hal_video_submit_scanline(buf);
    }

    fire_interrupts(system);
}

void galaga_debug_take_starvation(uint32_t *render_max_us, uint32_t *noblock_run_max) {
    if (render_max_us)   *render_max_us   = g_render_max_us;
    if (noblock_run_max) *noblock_run_max = g_noblock_run_max;
    g_render_max_us = 0;
    g_noblock_run_max = 0;
}

void galaga_init(galaga_system *system) {
    memset(system, 0, sizeof(*system));

    z80_init(&system->cpu_main);
    z80_init(&system->cpu_sub);
    z80_init(&system->cpu_sub2);
    galaga_ports_wire(system);

    galaga_51xx_init(&system->io51);
    galaga_54xx_init(&system->io54);

    // Screen rotation 3 (90 deg CW), NOT 1. This is the value that puts the
    // game upright on the same physically-rotated monitor that
    // ArcadeMachine_Invaders and ArcadeMachine_LunarRescue are upright on
    // at their own default of 1 -- confirmed on real hardware, where this
    // machine at rotation 1 came up 180 degrees off and needed two presses
    // of the ROTATE button to correct.
    //
    // The two families genuinely differ, so this is not a bug in either
    // renderer: rotation 1 and rotation 3 are implemented identically in
    // both (case 3 reverses both axes relative to case 1, i.e. an exact
    // 180). What differs is which end of each game's NATIVE raster is the
    // top of the player's screen, because the real cabinets mounted their
    // monitors in opposite orientations -- the 8080bw games one way, the
    // Namco games the other. An earlier version of this line copied
    // Invaders' default with the comment "same convention as Invaders",
    // which was precisely the wrong assumption.
    system->rotation = 3;
    system->mirror_x = false;

    hal_video_init();
}

bool galaga_load_assets(galaga_system *system, uint16_t *out_error_color) {
    galaga_rom_load_status_t rom_status = galaga_load_rom(system);
    if (rom_status == GALAGA_ROM_LOAD_NO_STORAGE) {
        *out_error_color = GALAGA_COLOR_ERROR_NO_CARD;
        return false;
    }
    if (rom_status == GALAGA_ROM_LOAD_NO_ROM_FILES) {
        *out_error_color = GALAGA_COLOR_ERROR_NO_ASSETS;
        return false;
    }

    galaga_video_build_caches();
    hal_storage_unmount();

    // Namco WSG audio (3-voice wavetable). The 54XX explosion/noise
    // channel mixes into the same fill callback -- see galaga_audio.cpp.
    hal_audio_init(GALAGA_AUDIO_SAMPLE_RATE);
    galaga_audio_init(system);

    hal_input_init();
    return true;
}

void galaga_run_frame(galaga_system *system) {
    if (system->rotation == 1 || system->rotation == 3) {
        run_frame_interleaved(system);
    } else {
        run_frame_sequential(system);
    }
}
