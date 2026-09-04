// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Pac-Man machine lifecycle -- orchestrates ArcadeCPU_Z80 + this machine's
// own port/video/audio/asset modules, talking to hardware only through
// ArcadeHAL. Same shape as invaders_machine.cpp/lrescue_machine.cpp.
#include <string.h>
#include "pacman_machine.h"
#include "pacman_ports.h"
#include "pacman_video.h"
#include "pacman_audio.h"
#include "pacman_assets.h"
#include "arcade_hal_video.h"
#include "arcade_video_geom.h"
#include "arcade_hal_audio.h"
#include "arcade_hal_storage.h"
#include "arcade_hal_input.h"

// Z80 clock 18.432MHz/6 = 3.072MHz, frame rate (18.432MHz/3)/(384*264) =
// 60.606060... Hz -- both verified against MAME's pacman() machine_config
// (see pacman_machine.h's header comment). Unlike ArcadeMachine_Invaders'
// CYCLES_PER_FRAME (a repeating decimal needing float math and a
// carry-forward `cyc` variable across frames), these two constants divide
// out to an exact integer: 3072000 * 384 * 264 / (18432000/3) = 50688
// cycles/frame, no remainder, no carry-forward needed.
#define PACMAN_CYCLES_PER_FRAME 50688UL

void pacman_init(pacman_system *system) {
    memset(system, 0, sizeof(*system));

    z80_init(&system->cpu);
    pacman_ports_wire(system);

    // Build the canvas mapping for this raster. Must happen before the
    // first frame -- pacman_video.cpp reads av_tate/av_yoko on every
    // scanline and they are all zeroes until this runs. LONG axis first
    // (GAME_WIDTH, 288), then SHORT (GAME_HEIGHT, 224); see
    // arcade_video_geom.h for why those names and not width/height.
    av_geom_init(PACMAN_GAME_WIDTH, PACMAN_GAME_HEIGHT);

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

bool pacman_load_assets(pacman_system *system, uint16_t *out_error_color) {
    pacman_rom_load_status_t rom_status = pacman_load_rom(system);
    if (rom_status == PACMAN_ROM_LOAD_NO_STORAGE) {
        *out_error_color = PACMAN_COLOR_ERROR_NO_CARD;
        return false;
    }
    if (rom_status == PACMAN_ROM_LOAD_NO_ROM_FILES) {
        *out_error_color = PACMAN_COLOR_ERROR_NO_ASSETS;
        return false;
    }

    pacman_video_build_caches();
    hal_storage_unmount();

    hal_audio_init(PACMAN_AUDIO_SAMPLE_RATE);
    pacman_audio_init(system);

    hal_input_init();
    return true;
}

// Runs the frame's cycles INTERLEAVED with scanline submission, evenly
// spreading PACMAN_CYCLES_PER_FRAME across the HAL_VIDEO_HEIGHT
// acquire/submit calls instead of running them all in one uninterrupted
// burst before the first call. Fixes arcade_arduino/DEVNOTES.md problem
// #19: even after fixing the renderer itself (problem #18), a real,
// visible stall remained because *this* loop still ran the whole frame's
// ~50,688 Z80 cycles before the frame renderer ever called
// hal_video_acquire_scanline() -- same starvation mechanism, just moved
// from the renderer into the CPU loop.
//
// THIS IS NOW THE ONLY PATH. It used to be gated to tate/CW, with
// landscape/180 falling back to a fully sequential run_frame_sequential()
// because a yoko scanline needs a native COLUMN and the renderer could only
// produce rows. pacman_video.cpp's render_native_column() removed that
// constraint, so the gate, the sequential path and the 129KB frame_cache
// are all gone together -- and with them the red those orientations showed
// every frame (DEVNOTES #18/#75).
//
// Side effect worth knowing about: because each scanline is now rendered
// from whatever VRAM/sprite state exists at that exact point in the
// frame's CPU execution (not the frame's *final* state), a scanline near
// the top of the picture can reflect slightly older game state than one
// near the bottom. This is not a new inaccuracy introduced by emulation --
// it is how real scanline-order CRT hardware actually behaves, and Pac-Man's
// sprites move only a few pixels per frame, so it should be imperceptible
// in practice; flag it if anything looks like a one-frame tear on fast-
// moving elements.
// `system->cpu.cyc` is the z80 core's own free-running uint32_t, never
// reset, so at Pac-Man's 3.072MHz it wraps roughly every 23 minutes.
// Comparing ELAPSED cycles (a subtraction) rather than an absolute target
// is what makes that safe indefinitely: unsigned subtraction wraps modulo
// 2^32 exactly as the counter does. DEVNOTES.md problem #22 is a real
// permanent hang from getting this wrong, and it is why every local here is
// uint32_t and not `long`.
static void run_frame_interleaved(pacman_system *system) {
    uint32_t start = system->cpu.cyc;

    for (uint32_t i = 0; i < HAL_VIDEO_HEIGHT; i++) {
        // Exact proportional target delta (not repeated addition) so the
        // final slice lands exactly on PACMAN_CYCLES_PER_FRAME elapsed
        // regardless of how that divides by the scanline count.
        uint32_t target_delta =
            (uint32_t)((uint64_t)PACMAN_CYCLES_PER_FRAME * (i + 1) / HAL_VIDEO_HEIGHT);
        while ((uint32_t)(system->cpu.cyc - start) < target_delta) {
            z80_step(&system->cpu);
        }

        uint16_t *buf = hal_video_acquire_scanline();
        pacman_video_render_scanline(system, i, buf);
        hal_video_submit_scanline(buf);
    }

    // One vblank interrupt per frame, fired at the end -- unchanged from
    // when this shared the job with a sequential path.
    if (system->interrupt_enable) {
        z80_gen_int(&system->cpu, system->interrupt_vector);
    }
}

void pacman_run_frame(pacman_system *system) {
    // Every rotation, one path. See run_frame_interleaved()'s comment for
    // why there is no longer a sequential fallback for landscape/180.
    run_frame_interleaved(system);
}
