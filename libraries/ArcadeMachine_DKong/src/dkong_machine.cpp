// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Donkey Kong machine lifecycle -- orchestrates ArcadeCPU_Z80 + this
// machine's own port/video/audio/asset modules, talking to hardware only
// through ArcadeHAL. Same shape as pacman_machine.cpp.
#include <string.h>
#include "dkong_machine.h"
#include "dkong_ports.h"
#include "dkong_video.h"
#include "dkong_audio.h"
#include "dkong_assets.h"
#include "arcade_hal_video.h"
#include "arcade_video_geom.h"
#include "arcade_hal_audio.h"
#include "arcade_hal_storage.h"
#include "arcade_hal_input.h"
#include <Arduino.h> // micros() for the audio-cost measurement below

// Z80 clock CLOCK_1H = MASTER_CLOCK/5/4 = 61.44MHz/20 = 3.072MHz, and the
// frame rate is PIXEL_CLOCK/(HTOTAL*VTOTAL) = 6.144MHz/(384*264) =
// 60.606060... Hz -- all from MAME's dkong.h constants as used by
// dkong_base(). Those divide out to an exact integer with no remainder:
//
//   3,072,000 / (6,144,000 / (384 * 264)) = 50,688 cycles per frame
//
// Which is, to the cycle, the same budget ArcadeMachine_Pacman has. That is
// not a coincidence worth glossing over -- both boards derive video and CPU
// timing from the same family of Nintendo/Namco-era dividers -- but it IS a
// coincidence that they landed on identical numbers from different master
// clocks (18.432MHz there, 61.44MHz here), so this is derived here rather
// than borrowed from that file.
#define DKONG_CYCLES_PER_FRAME 50688UL


void dkong_init(dkong_system *system) {
    memset(system, 0, sizeof(*system));

    z80_init(&system->cpu);

    // Build the canvas mapping for this raster (arcade_video_geom.h). Must
    // happen before the first frame -- the renderer reads av_tate/av_yoko on
    // every scanline and they are all zeroes until this runs. LONG axis
    // first (GAME_WIDTH), then SHORT (GAME_HEIGHT).
    av_geom_init(DKONG_GAME_WIDTH, DKONG_GAME_HEIGHT);

    dkong_ports_wire(system);

    // Rotation 1 (90 deg CCW), NOT 3 -- even though Pac-Man and Ms. Pac-Man
    // both default to 3 and this is, like theirs, a portrait cabinet.
    //
    // DEVNOTES.md problem #33 is the record of assuming a rotation default
    // carries over from a neighbouring game; this file initially repeated
    // that assumption in the opposite direction and came out 180 degrees
    // off. The invariant that actually holds across every orientation
    // confirmed on hardware here is stated in terms of the framebuffer, not
    // in terms of which game it is:
    //
    //     the TOP of the game's picture must land on the RIGHT-hand side
    //     of the DVI framebuffer.
    //
    // Space Invaders reaches that at rotation 1, Pac-Man and Ms. Pac-Man at
    // rotation 3, and Donkey Kong at rotation 1 -- three different machines,
    // two different values, one physical result. The value differs because
    // the machines' NATIVE raster orientations differ, which is a fact about
    // how each real cabinet mounted its monitor. It is not a house style,
    // and it cannot be inferred from the manufacturer or from the cabinet
    // being portrait.
    //
    // Checked by rendering the same frame at both values in
    // tools/dkong_host/ and comparing where the score text lands against a
    // known-good frame from another game -- see DEVNOTES.md problem #41.
    system->rotation = 1;
    system->mirror_x = false;

    // Everything else is zero, and for this machine that is meaningful
    // rather than incidental: nmi_mask starts clear (the ROM enables it),
    // the DMA is idle, and because this board's inputs are ACTIVE HIGH the
    // zeroed in0/in1/in2 are already the correct "nothing pressed" state.

    hal_video_init();
}

bool dkong_load_assets(dkong_system *system, uint16_t *out_error_color) {
    dkong_rom_load_status_t rom_status = dkong_load_rom(system);
    if (rom_status == DKONG_ROM_LOAD_NO_STORAGE) {
        *out_error_color = DKONG_COLOR_ERROR_NO_CARD;
        return false;
    }
    if (rom_status == DKONG_ROM_LOAD_NO_ROM_FILES) {
        *out_error_color = DKONG_COLOR_ERROR_NO_ASSETS;
        return false;
    }

    dkong_video_build_caches();
    hal_storage_unmount();

    hal_audio_init(DKONG_AUDIO_SAMPLE_RATE);
    dkong_audio_init(system);

    hal_input_init();
    return true;
}

// Runs the frame's cycles INTERLEAVED with scanline submission, spreading
// DKONG_CYCLES_PER_FRAME evenly across the HAL_VIDEO_HEIGHT
// acquire/submit calls rather than running them all in one burst before the
// first call. This is the fix DEVNOTES.md problems #20/#34/#36 applied to
// every other game here, built in from the start rather than back-applied:
// Core 1 can only coast on the 8-buffer scanline queue (~555us, a hard
// libdvi ceiling) plus vertical blanking, and a whole frame's emulation
// exceeds that.
//
// Only safe for tate/CW rotation, whose renderer reads live VRAM on demand
// one row at a time. Landscape/180 need the frame's final state before they
// can emit even one physical scanline, so they keep the fully-sequential
// path -- the same known, deprioritised limitation ArcadeMachine_Pacman has
// (and the same red lines in those two orientations).
//
// `system->cpu.cyc` is a free-running uint32_t the core never resets, so at
// 3.072MHz it wraps roughly every 23 minutes. Comparing ELAPSED cycles (a
// subtraction) rather than an absolute target is what makes that safe
// indefinitely -- unsigned subtraction wraps modulo 2^32 exactly as the
// counter does. DEVNOTES.md problem #22 is a real permanent hang caused by
// getting this wrong, and #26/#27 are why every local here is uint32_t and
// not `long`.
static void run_frame_interleaved(dkong_system *system) {
    uint32_t start = system->cpu.cyc;

    // Landscape renders a raster COLUMN per canvas scanline and needs this
    // frame's sprites latched and bucketed first (dkong_video.cpp). Tate
    // arbitrates per scanline inside render_native_row() and skips it.
    if (system->rotation == 0 || system->rotation == 2)
        dkong_video_begin_frame(system);

    for (uint32_t i = 0; i < HAL_VIDEO_HEIGHT; i++) {
        uint32_t target_delta =
            (uint32_t)((uint64_t)DKONG_CYCLES_PER_FRAME * (i + 1) / HAL_VIDEO_HEIGHT);
        while ((uint32_t)(system->cpu.cyc - start) < target_delta) {
            z80_step(&system->cpu);
        }

        uint16_t *buf = hal_video_acquire_scanline();
        dkong_video_render_scanline(system, i, buf);
        hal_video_submit_scanline(buf);

        // A slice of this frame's sound, here rather than after the loop:
        // see dkong_audio_run_slice()'s comment for what running it all at
        // the end does to the DVI queue.
        dkong_audio_run_slice(i, HAL_VIDEO_HEIGHT);
    }
}

// run_frame_sequential() was deleted with the frame cache (DEVNOTES #92).
// It ran a whole frame of CPU and then a whole frame of rendering before the
// first scanline was submitted, which is exactly the shape that starves the
// DVI queue. dkong_draw_frame() still exists for the standalone,
// no-CPU-to-interleave case documented in dkong_video.h.

void dkong_run_frame(dkong_system *system) {
    // EVERY rotation interleaves now. Landscape used to take
    // run_frame_sequential(), which ran a whole frame of CPU and then
    // rendered all 224 raster rows into a 114KB cache before submitting a
    // single scanline. Two bursts back to back, against 2,032us of queue
    // runway -- which is why both landscape rotations were roughly 3/4 red
    // on a real screen. See DEVNOTES #92.
    run_frame_interleaved(system);

    // The vblank interrupt is an NMI, not an IRQ, and it is gated by a
    // software mask the ROM writes to 0x7D84 -- MAME's vblank_irq():
    //     if (state && m_nmi_mask) set_input_line(INPUT_LINE_NMI, ASSERT_LINE)
    // A build that fires this unconditionally will look like it works for a
    // while and then behave strangely wherever the game deliberately masks
    // the NMI off; a build that never fires it does nothing at all after
    // the title screen.
    if (system->nmi_mask) {
        z80_gen_nmi(&system->cpu);
    }

    // Sound runs once per frame, after the main CPU has had its turn --
    // the two communicate only through latches, so they do not need to be
    // interleaved. Deliberately AFTER the frame's emulation rather than
    // before: this way a command the main CPU wrote during this frame is
    // already in the latch when the 8035 next looks. See dkong_audio.cpp
    // for why it paces itself off the audio FIFO rather than the frame.
    // How long the sound half actually costs, measured rather than
    // inferred. Switching sound on took `work` from ~9.4ms to ~13.6ms of a
    // 16.66ms budget and broke frame pacing; attributing that to the 8035
    // without measuring it would be exactly the mistake this project keeps
    // paying for.
    // The interleaved path has already produced this frame's audio inside
    // the scanline loop; only the sequential (landscape/180) path needs a
    // whole-frame call here.
    if (system->rotation != 1 && system->rotation != 3) {
        dkong_audio_run_frame(system);
    }
}

uint32_t dkong_debug_audio_us(void) { return dkong_audio_debug_cost_us(); }
