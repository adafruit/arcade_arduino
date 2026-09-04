// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Burger Time machine lifecycle -- orchestrates two ArcadeCPU_M6502 cores
// plus this machine's own port/video/audio/asset modules, talking to
// hardware only through ArcadeHAL. Same shape as dkong_machine.cpp.
#include <string.h>
#include "btime_machine.h"
#include "btime_ports.h"
#include "btime_video.h"
#include "btime_audio.h"
#include "btime_assets.h"
#include "arcade_hal_video.h"
#include "arcade_video_geom.h"
#include "arcade_hal_audio.h"
#include "arcade_hal_storage.h"
#include "arcade_hal_input.h"
#include <Arduino.h> // micros(), for the cost breakdown below

// THE COST BREAKDOWN IS OFF BY DEFAULT, and that is not laziness -- it costs
// about 1,300 micros() calls per frame (three per scanline plus two per
// submitted one), which measured ~0.8ms of a 16.66ms budget on device. It
// was indispensable for finding where this port's time went (DEVNOTES.md
// #59/#60) and it stays in the tree for next time, but leaving it enabled
// would mean shipping 5% of the frame to measure the frame.
//
// Set to 1 and reflash when a number is needed; the sketch prints
// cpu/render/snd in its heartbeat either way (zeros when disabled).
#ifndef BTIME_COST_PROFILING
#define BTIME_COST_PROFILING 0
#endif

#if BTIME_COST_PROFILING
#define COST_NOW() micros()
#define COST_ADD(acc, t0) do { (acc) += micros() - (t0); } while (0)
#else
#define COST_NOW() 0u
#define COST_ADD(acc, t0) do { (void)(t0); } while (0)
#endif

// Interrupts actually delivered to the sound CPU, for the counters below.
// A silent machine with zero NMIs and a silent machine with 16,000 NMIs are
// entirely different problems, and neither is visible from the outside.
static uint32_t g_sound_nmis;
static uint32_t g_sound_irqs;
static uint32_t g_main_irqs;
static uint32_t g_main_irq_windows;

// Frame cost breakdown -- see btime_debug_take_costs().
static uint32_t g_cpu_us, g_render_us, g_audio_us;      // this frame
static uint32_t g_cpu_sum, g_render_sum, g_audio_sum;   // window
static uint32_t g_cost_frames;
static uint32_t g_cpu_mean, g_render_mean, g_audio_mean;

static void cost_frame_done(void) {
    g_cpu_sum += g_cpu_us; g_render_sum += g_render_us; g_audio_sum += g_audio_us;
    g_cpu_us = g_render_us = g_audio_us = 0;
    if (++g_cost_frames >= 60u) {
        g_cpu_mean = g_cpu_sum / g_cost_frames;
        g_render_mean = g_render_sum / g_cost_frames;
        g_audio_mean = g_audio_sum / g_cost_frames;
        g_cpu_sum = g_render_sum = g_audio_sum = 0;
        g_cost_frames = 0;
    }
}

void btime_init(btime_system *system) {
    memset(system, 0, sizeof(*system));

    m6502_init(&system->cpu);
    m6502_init(&system->audiocpu);
    btime_ports_wire(system);

    // Build the canvas mapping for this raster (arcade_video_geom.h). Must
    // happen before the first frame -- the renderer reads av_tate/av_yoko on
    // every scanline and they are all zeroes until this runs. LONG axis
    // first (GAME_WIDTH), then SHORT (GAME_HEIGHT).
    av_geom_init(BTIME_GAME_WIDTH, BTIME_GAME_HEIGHT);

    // ASPECT CORRECTION DELIBERATELY OFF FOR THIS GAME, and it is the one
    // that needs it most: the raster is square (240x240), so at 1:1 the
    // picture is 33.3% too wide for its height in BOTH orientations,
    // against Pac-Man's 3.7% in tate.
    //
    // It does not fit. Measured on hardware (DEVNOTES #81), tate: render
    // 4950us at 1:1 against 6624us corrected, on a frame whose other costs
    // (CPU 6.9ms, sound 3.3ms) leave about 1.1ms spare. Corrected,
    // `work_max` is 17172us of a 16660us budget and the DVI queue starves
    // ~240 times a frame. Yoko is marginal rather than broken -- 15783us
    // and ~50 starves -- but tate is this game's default.
    //
    // The cost is structural, not a missing optimisation: at 1:1 this
    // renderer emits two pixels per 32-bit store, unrolled by four, and a
    // 240->320 upsample cannot use either trick. Both the destination-driven
    // and source-driven forms were measured; the source-driven one (which
    // ships) is 635us better and still not enough.
    //
    // Turn it on with -DTEST_STRETCH=1 to see the correct proportions and
    // the starvation together. Burger Time and Donkey Kong are now the two
    // games that need the correction and cannot afford it (#78).
    av_geom_set_stretch(false);


    // ROTATION 1 (90 deg CCW), predicted from MAME and still to be
    // confirmed against the framebuffer invariant.
    //
    // DEVNOTES.md problems #33 and #41 are the record of assuming this
    // value carries over from a neighbouring game, once in each direction.
    // The invariant that actually holds is about the framebuffer, not about
    // which game it is:
    //
    //     the TOP of the game's picture must land on the RIGHT-hand side
    //     of the DVI framebuffer.
    //
    // What is new here is that there IS a reliable predictor: MAME's ROT
    // flag in the driver's GAME() line. Checked against every game in this
    // project that is confirmed on hardware, it is six for six --
    //
    //     ROT270 -> 1 : Space Invaders, Lunar Rescue, Donkey Kong
    //     ROT90  -> 3 : Pac-Man, Ms. Pac-Man, Galaga
    //
    // -- and btime's GAME() line says ROT270, so 1. That does not
    // contradict "cannot be copied from a neighbour" (neighbours genuinely
    // differ, which is why the DRIVER rather than the neighbour is the
    // source); it does mean the starting guess is evidence-based rather
    // than a coin flip. It is still a prediction: render both candidates in
    // tools/btime_host/ and check where the score text lands before
    // trusting it.
    system->rotation = 1;
    system->mirror_x = false;

    // Inputs are ACTIVE LOW on this board (except the coin bits), so the
    // memset above leaves every button reading as HELD until the first
    // btime_input_update(). Set the idle state explicitly rather than
    // relying on the caller getting there first -- this is the trap
    // recorded in tools/host_common/hal_host.cpp for Galaga, where all-zero
    // shadow bytes read as a permanently inserted coin.
    system->p1 = 0xFF;
    system->p2 = 0xFF;
    system->system_in = 0x3F; // start/tilt idle high, coin bits idle LOW

    // Everything else zero is meaningful here: audio_nmi_en starts CLEAR
    // (machine_reset() does m_audionmi->in_w<0>(0) and the ROM enables it),
    // sound_irq starts clear, and had_written starts clear.

    hal_video_init();
}

bool btime_load_assets(btime_system *system, uint16_t *out_error_color) {
    btime_rom_load_status_t status = btime_load_rom(system);
    if (status == BTIME_ROM_LOAD_NO_STORAGE) {
        *out_error_color = BTIME_COLOR_ERROR_NO_CARD;
        return false;
    }
    if (status == BTIME_ROM_LOAD_NO_ROM_FILES) {
        *out_error_color = BTIME_COLOR_ERROR_NO_ASSETS;
        return false;
    }

    btime_video_build_caches();
    hal_storage_unmount();

    // Only now that the ROM is in place can either CPU be reset: a 6502
    // takes its initial PC from 0xFFFC/0xFFFD, so resetting an unloaded
    // machine would start executing at whatever address zeroed memory
    // points to.
    btime_ports_reset_cpus(system);

    hal_audio_init(BTIME_AUDIO_SAMPLE_RATE);
    btime_audio_init();

    hal_input_init();
    return true;
}

// Advances one game scanline's worth of both CPUs and of the machine's
// video/sound timing. Everything this board does per-scanline comes off one
// counter, which is the honest structure because on the real hardware all
// of it comes off one set of dividers from the same crystal:
//
//   - the VBLANK FLAG the main program polls at 0x4003 bit 7, since this
//     board has no vblank interrupt at all;
//   - the sound CPU's NMI, from a scanline timer;
//   - the audio slice boundary.
static void run_scanline(btime_system *system, uint32_t line) {
    const uint32_t cost_t0 = COST_NOW();
    // Visible raster lines are 8..247 of 272 (set_raw's vbend/vbstart).
    system->vblank = (line < BTIME_FIRST_VISIBLE_LINE) ||
                     (line >= BTIME_FIRST_VISIBLE_LINE + BTIME_GAME_HEIGHT);

    // --- main CPU ---
    // Its ONLY interrupt is the coin insert; see btime_ports_coin_irq().
    //
    // THE PENDING FLAG IS HELD UNTIL THE CPU CAN ACTUALLY TAKE IT, and that
    // detail is the whole ballgame. MAME raises this with
    //
    //     m_maincpu->set_input_line(0, HOLD_LINE);
    //
    // and HOLD_LINE means "assert the line and clear it automatically when
    // the CPU ACKNOWLEDGES the interrupt" -- not "pulse it once". Burger
    // Time's main loop runs with the I flag SET almost all of the time and
    // opens the window only briefly, so a one-shot that fires on the next
    // scanline boundary regardless finds interrupts masked, is swallowed by
    // m6502_gen_irq()'s own `if (idf == 0)` guard, and the coin is lost
    // forever. That was this port's first real bug: the game ignored coins
    // completely and sat in its attract demo, which renders a full playfield
    // with moving sprites and therefore looks exactly like a working game.
    // It was caught by running the SAME sequence with no coin at all and
    // getting byte-identical counters and sprite positions -- the control
    // run the playbook insists on.
    // AND IT IS CHECKED BETWEEN EVERY INSTRUCTION, not once per scanline.
    // That is not a refinement, it is the difference between working and
    // not. Disassembling where the main CPU actually sits shows why:
    //
    //     CA32: 58        CLI
    //     CA33: EA EA EA EA   NOP x4
    //     CA37: 78        SEI
    //
    // The program runs with interrupts masked and opens a window about TEN
    // CYCLES wide once per frame. A scanline is 96 cycles, so an interrupt
    // check that happens only at scanline boundaries lands inside that
    // window roughly never -- the coin IRQ is offered while I is still set,
    // m6502_gen_irq()'s own guard drops it, and no coin is ever accepted.
    // Sampling the same flag per scanline also made the diagnostic lie:
    // "unmasked scanlines: 0" was a sampling artifact, not proof that the
    // program never enables interrupts.
    //
    // Checking per instruction is also simply what the hardware does -- a
    // 6502 samples its IRQ line at every instruction boundary. The cost is
    // one bool test per instruction, and the body only runs while a coin is
    // genuinely pending.
    // TWO LOOPS, chosen once per scanline. The interrupt-checking version
    // is only needed while a coin is actually pending, and a coin can only
    // become pending in btime_input_update() between frames -- never inside
    // this loop. So the common case gets a tight loop with no per-
    // instruction test at all, and the flag is not reloaded from the system
    // struct 96 cycles' worth of instructions in a row.
    {
        const uint32_t start = system->cpu.cyc;
        if (!system->coin_irq_pending) {
            while ((uint32_t)(system->cpu.cyc - start) < BTIME_MAIN_CYCLES_PER_LINE) {
                m6502_step(&system->cpu);
            }
        } else {
            while ((uint32_t)(system->cpu.cyc - start) < BTIME_MAIN_CYCLES_PER_LINE) {
                if (system->coin_irq_pending) {
                    if (!system->cpu.idf) {
                        system->coin_irq_pending = false;
                        m6502_gen_irq(&system->cpu);
                        g_main_irqs++;
                    } else {
                        g_main_irq_windows++; // instructions spent waiting for CLI
                    }
                }
                m6502_step(&system->cpu);
            }
        }
    }

    // --- sound CPU ---
    // Two interrupt sources, and they behave differently:
    //
    //  1. IRQ, LEVEL-triggered: asserted while the sound latch holds an
    //     unread command (btime_ports.cpp sets sound_irq on the main CPU's
    //     write and clears it on the sound CPU's read of 0xA000, which is
    //     what generic_latch_8_device::read() does). Checked every scanline
    //     rather than once, because the CPU may have interrupts masked when
    //     the command arrives.
    //  2. NMI, EDGE-triggered: MAME merges a software enable with bit 3 of
    //     the scanline counter --
    //         audio_nmi_gen(): m_audionmi->in_w<1>((scanline & 8) >> 3)
    //         INPUT_MERGER_ALL_HIGH -> the CPU's NMI line
    //     -- so the line is high for 8 scanlines and low for 8, giving one
    //     RISING EDGE every 16 scanlines = 15625/16 = 976.56 Hz. Modelling
    //     this as a level instead would re-enter the NMI handler for eight
    //     straight scanlines and the sound CPU would never run anything
    //     else. The enable is bit 0 of a write to 0xC000-0xDFFF and starts
    //     CLEAR, so a build that hardcodes it on runs the tick before the
    //     ROM is ready for it.
    const bool nmi_line = system->audio_nmi_en && ((line & 8u) != 0);
    if (nmi_line && !system->audio_nmi_prev) {
        // NMI is not maskable, so delivering it at the scanline boundary is
        // faithful to within one instruction.
        m6502_gen_nmi(&system->audiocpu);
        g_sound_nmis++;
    }
    system->audio_nmi_prev = nmi_line;

    {
        // Same split as the main CPU above. The sound CPU's IRQ is a LEVEL
        // held while the latch holds an unread command, and it can only be
        // taken at an instruction boundary with I clear -- so it does need
        // a per-instruction check WHILE PENDING. But it cannot become
        // pending inside this loop: only the main CPU's write to 0x4003
        // raises it, and the main CPU has already had its turn for this
        // scanline. So the idle case gets the tight loop.
        const uint32_t start = system->audiocpu.cyc;
        if (!system->sound_irq) {
            while ((uint32_t)(system->audiocpu.cyc - start) < BTIME_SOUND_CYCLES_PER_LINE)
                m6502_step(&system->audiocpu);
        } else {
            while ((uint32_t)(system->audiocpu.cyc - start) < BTIME_SOUND_CYCLES_PER_LINE) {
                // Only counts as delivered when the CPU could actually take
                // it -- counting the attempt would report a busy sound CPU
                // that is in fact ignoring everything.
                if (system->sound_irq && !system->audiocpu.idf) {
                    g_sound_irqs++;
                    m6502_gen_irq(&system->audiocpu);
                }
                m6502_step(&system->audiocpu);
            }
        }
    }

    COST_ADD(g_cpu_us, cost_t0);
}

// Runs the frame's cycles INTERLEAVED with scanline submission, rather than
// running the whole frame and then drawing. Core 1 can only coast on the
// 8-buffer scanline queue (~555us, a hard libdvi ceiling) plus vertical
// blanking, and a whole frame's emulation exceeds that -- this is the fix
// DEVNOTES.md #20/#34/#36 applied to every other game here, built in from
// the start.
//
// The mapping is unusually tidy on this machine: 240 of the 272 game
// scanlines are visible, and HAL_VIDEO_HEIGHT is also 240, so
// visible game line <-> submitted DVI row is 1:1 and the 32 blanking lines
// carry CPU cycles and audio but no output.
//
// Only safe for tate/CW rotation, whose renderer reads live VRAM one row at
// a time. Landscape/180 need the frame's final state before they can emit
// even one physical scanline, so they keep the fully-sequential path -- the
// same known, deprioritised limitation ArcadeMachine_Pacman has.
static void run_frame_interleaved(btime_system *system) {
    uint32_t submitted = 0;

    for (uint32_t line = 0; line < BTIME_SCANLINES_PER_FRAME; line++) {
        run_scanline(system, line);

        // SUBMISSIONS ARE SPREAD OVER ALL 272 GAME SCANLINES, not just the
        // 240 visible ones -- and that distinction turned out to matter a
        // great deal.
        //
        // The obvious structure is "submit on the visible lines, run the
        // blanking lines bare", and that is what this did first. But the
        // 32 blanking lines still execute both CPUs and their audio, which
        // is about 1.9ms of work with NO scanline handed to Core 1. Core 1
        // can only coast on the 8-buffer queue (~555us, a hard libdvi
        // ceiling) plus vertical blanking, so it starved every single
        // frame. The symptom was a `frame` of 18.3ms while `work` was only
        // 16.4ms and `blocked` was 1.9ms: Core 0 ran ahead, ran the gap,
        // and then sat waiting -- with the queue empty in between.
        //
        // This is DEVNOTES.md #18/#20/#34/#36/#48 for the SIXTH time, in a
        // new disguise: not "is there budget" but "is there ever a gap
        // longer than ~2ms between two submissions". Distributing the 240
        // submissions evenly across all 272 lines removes the gap entirely.
        //
        // The rendered row is `submitted`, so it can lag the emulated beam
        // line by up to 12% of a frame. That is deliberate and harmless --
        // the alternative is the starvation above, and every renderer here
        // already reads live VRAM mid-frame by design.
        const uint32_t want = ((line + 1u) * HAL_VIDEO_HEIGHT)
                              / BTIME_SCANLINES_PER_FRAME;
        while (submitted < want && submitted < HAL_VIDEO_HEIGHT) {
            uint16_t *buf = hal_video_acquire_scanline();
            const uint32_t r0 = COST_NOW();
            btime_video_render_scanline(system, submitted, buf);
            COST_ADD(g_render_us, r0);
            hal_video_submit_scanline(buf);

            const uint32_t a0 = COST_NOW();
            btime_audio_run_slice(submitted, HAL_VIDEO_HEIGHT);
            COST_ADD(g_audio_us, a0);
            submitted++;
        }
    }
}

// Landscape/180. The renderer needs the frame's final state before it can
// emit even one physical scanline (each of those scanlines is a raster
// COLUMN), so the drawing cannot be interleaved -- the same known,
// deprioritised limitation ArcadeMachine_Pacman has in these orientations.
//
// THE AUDIO STILL IS interleaved, and that is not a detail. An earlier
// version of this function ran only the CPUs and then drew, exactly
// mirroring dkong_machine.cpp's shape -- which meant
// btime_audio_run_slice() was never called at all in these two rotations
// and the game was SILENT in them. Nothing about the picture changes, so
// only a number shows it: 441,000 ring underruns against 0 in tate. Caught
// by checking the audio counters in ALL FOUR rotations rather than in the
// default one, which is the same discipline as running the no-coin control
// (DEVNOTES.md #51).
//
// Slicing off the scanline loop here rather than generating a whole frame's
// audio in one call afterwards also keeps the #48 rule intact for free: no
// single slice is a long uninterrupted burst.
void btime_run_frame(btime_system *system) {
    // Every rotation, one path. btime_video.cpp's render_native_column()
    // removed the reason landscape/180 ever needed a whole-frame burst
    // (DEVNOTES #79), and with it the frame_pen cache and the separate
    // sequential loop that fed it.
    run_frame_interleaved(system);
    cost_frame_done();

    // NOTE the absence of anything here. Every other machine in this
    // project fires a vblank interrupt at this point; this one has none to
    // fire. The program has already learned where the beam is by polling
    // 0x4003 bit 7 inside run_scanline(). If this port ever appears to hang
    // on a black screen, that read -- not a missing interrupt -- is the
    // thing to instrument (btime_debug_take_counters()).
}

void btime_debug_take_costs(uint32_t *out_cpu_us, uint32_t *out_render_us,
                            uint32_t *out_audio_us) {
    if (out_cpu_us)    *out_cpu_us    = g_cpu_mean;
    if (out_render_us) *out_render_us = g_render_mean;
    if (out_audio_us)  *out_audio_us  = g_audio_mean;
}

void btime_debug_take_counters(const btime_system *system,
                               btime_counters *out) {
    if (!out) return;
    btime_ports_take_counters(&out->vblank_reads, &out->opcode_swaps,
                              &out->mirror_reads, &out->latch_writes,
                              &out->system_reads, &out->latch_reads);
    out->main_irqs = g_main_irqs;
    out->main_irq_windows = g_main_irq_windows;
    g_main_irqs = 0;
    g_main_irq_windows = 0;
    out->sound_nmis = g_sound_nmis;
    out->sound_irqs = g_sound_irqs;
    g_sound_nmis = 0;
    g_sound_irqs = 0;
    out->ay_reg_writes = btime_audio_debug_take_reg_writes();
    // ArcadeCPU_M6502 owns this one and keeps a running total per core that
    // it never resets, so report the sum of both CPUs rather than a
    // per-call delta. Should stay 0: MAME needs an undocumented-opcode
    // patch for Zoar on this same board and none for Burger Time.
    out->illegal_ops = system->cpu.illegal_ops + system->audiocpu.illegal_ops;
}
