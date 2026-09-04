// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Space Invaders machine lifecycle -- ported from invaders_pico's arcade.c.
// Orchestrates ArcadeCPU_i8080 + this machine's own port/video/audio/asset
// modules, talking to hardware only through ArcadeHAL.
#include <string.h>
#include "invaders_machine.h"
#include "invaders_ports.h"
#include "invaders_video.h"
#include "invaders_audio.h"
#include "invaders_assets.h"
#include "arcade_hal_video.h"
#include "arcade_hal_audio.h"
#include "arcade_hal_storage.h"
#include "arcade_hal_input.h"

#define FRAMERATE          59.541985
#define CYCLES_PER_FRAME   (1996800.0 / FRAMERATE) // ~33,536 cycles/frame

void invaders_init(arcade_system *system) {
    memset(&system->state, 0, sizeof(system->state));

    // Space Invaders' original PCB incompletely decodes its address bus,
    // aliasing RAM at 0x2000-0x3fff onto 0x4000-0x5fff; the self-test code
    // relies on this. See i8080.h's Cpu_state.mirror_2000_at_4000 doc comment
    // -- other 8080bw-family games (e.g. Lunar Rescue) must NOT set this.
    system->state.mirror_2000_at_4000 = true;

    system->left   = 0;
    system->right  = 0;
    system->shot   = 0;
    system->start1 = 0;
    system->start2 = 0;
    system->coin   = 0;
    system->tilt   = 0;

    system->ext_shift_offset = 0;
    system->ext_shift_data   = 0;
    system->cocktail_vertical_screen_flip = 0;
    system->rotation = 1;   // default: 90 deg CCW tate mode
    system->mirror_x = false;

    hal_video_init();
    invaders_ports_bind(system);
}

bool invaders_load_assets(arcade_system *system, uint16_t *out_error_color) {
    invaders_rom_load_status_t rom_status = invaders_load_rom(system);
    if (rom_status == INVADERS_ROM_LOAD_NO_STORAGE) {
        *out_error_color = INVADERS_COLOR_ERROR_NO_CARD;
        return false;
    }
    if (rom_status == INVADERS_ROM_LOAD_NO_ROM_FILES) {
        *out_error_color = INVADERS_COLOR_ERROR_NO_ASSETS;
        return false;
    }

    hal_audio_init(INVADERS_AUDIO_SAMPLE_RATE);
    int samples_loaded = invaders_audio_load_samples();
    hal_storage_unmount();
    if (samples_loaded == 0) {
        *out_error_color = INVADERS_COLOR_ERROR_NO_ASSETS;
        return false;
    }

    hal_input_init();
    return true;
}

// One emulated instruction plus the two per-frame interrupt checks --
// lifted VERBATIM out of invaders_run_frame()'s old single loop when that
// loop was split to interleave scanline output (see the comment there).
// Factored into one place precisely so the two call sites cannot drift
// apart from each other, or from the original semantics: `cyc` is an
// absolute running cycle count and both thresholds are absolute, so the
// mid-frame (RST1) and vblank (RST2) interrupts fire at exactly the same
// emulated instants as before the split.
static inline void step_cpu(arcade_system *system, int *cyc, int *int_state) {
    *cyc += exec_opcode(&system->state);
    if (*cyc >= (int)(CYCLES_PER_FRAME / 2) && *int_state == 0) {
        *int_state = 1;
        *cyc += interrupt(&system->state, 1);
    }
    if (*cyc >= (int)CYCLES_PER_FRAME && *int_state == 1) {
        *int_state = 2;
        *cyc += interrupt(&system->state, 2);
    }
}

void invaders_run_frame(arcade_system *system) {
    // `cyc` persists across frames -- any cycles run past this frame's
    // budget are carried forward and subtracted from the next frame's
    // budget, matching the reference clone's timing exactly. Its arithmetic,
    // and both interrupts' absolute cycle thresholds, are UNCHANGED by the
    // interleaving below: `cyc` is still an absolute running count, so
    // `CYCLES_PER_FRAME / 2` and `CYCLES_PER_FRAME` still fire at exactly
    // the same emulated instants they always did.
    static int cyc = 0;
    int int_state = 0;

    // INTERLEAVED CPU + scanline output. This function used to run the
    // whole frame's emulation in one uninterrupted loop and only then call
    // invaders_draw_frame(), i.e. with ZERO
    // hal_video_acquire_scanline()/hal_video_submit_scanline() calls until
    // every cycle was done -- the same shape DEVNOTES.md problem #20 fixed
    // for Pac-Man and problem #34 fixed for Lunar Rescue. During that burst
    // Core 1 can only coast on the 8-buffer scanline queue (~555us, a hard
    // ceiling in the vendored libdvi) plus the vertical blanking interval
    // (~1.4ms): about 2ms of cover for a ~1.8ms burst, i.e. ~200us of
    // margin, which a single audio ISR invocation can exceed outright. That
    // is what produced Lunar Rescue's red lines under heavy audio. This game
    // has the identical structure and the identical margin; it has never
    // shown the symptom only because its audio is far lighter (short WAV
    // samples triggered by port writes, no synthesized speaker channel), so
    // this is the same fix applied before the symptom rather than after.
    //
    // Like Lunar Rescue and unlike Pac-Man, no second sequential path is
    // needed for any rotation: invaders_video.cpp's render_scanline() reads
    // live VRAM on demand for all four rotations and keeps no frame cache,
    // so every rotation can interleave.
    //
    // Consequence, and it is the intended one: a scanline now renders from
    // whatever VRAM state exists at that point in the frame's execution
    // rather than the frame's final state. That is MORE faithful, not less
    // -- real scanline-order CRT hardware behaves exactly this way -- and
    // the same change on Pac-Man and Lunar Rescue produced no visible
    // tearing.
    const int cyc_start = cyc;
    const int cyc_total = (int)CYCLES_PER_FRAME - cyc_start;

    for (uint32_t i = 0; i < HAL_VIDEO_HEIGHT; i++) {
        // Exact proportional target (not repeated addition) so the final
        // slice lands precisely on CYCLES_PER_FRAME however that divides by
        // the scanline count -- same shape as lrescue_machine.cpp's and
        // pacman_machine.cpp's interleaved loops.
        // Deliberately 32-bit: cyc_total is ~33,500 and the scanline count
        // 240, so the product peaks around 8e6 and cannot overflow int32.
        // An int64 divide here would be a flash-resident library call run
        // 240 times per frame -- precisely the cost DEVNOTES.md problem #17
        // warns about, measured at ~700us/frame when it was written that way
        // in Lunar Rescue first (problem #34).
        int target = cyc_start +
            (cyc_total * (int)(i + 1)) / (int)HAL_VIDEO_HEIGHT;
        while (int_state != 2 && cyc < target) {
            step_cpu(system, &cyc, &int_state);
        }

        uint16_t *buf = hal_video_acquire_scanline();
        invaders_video_render_scanline(i, buf, system);
        hal_video_submit_scanline(buf);
    }

    // Integer rounding above can leave the last few cycles (and therefore
    // the vblank interrupt) unrun. Finish them.
    while (int_state != 2) {
        step_cpu(system, &cyc, &int_state);
    }

    cyc = (int)CYCLES_PER_FRAME - cyc;
}
