// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// invaders_fruitjam -- Space Invaders (ArcadeMachine_Invaders) on the
// Adafruit Fruit Jam (ArcadeBoard_FruitJam). This sketch is the SAMP
// composition root: it is the ONLY place that knows both "this game" and
// "this board" at once. Swapping either axis means writing a different
// sketch like this one, not editing the Machine or Board library.
//
// Core 0: game emulation, input polling, board-to-game input mapping.
// Core 1: hal_video_run() -- drives the DVI signal; never returns.
//
// Boot order matters (ported from invaders_pico's main.c, which documents
// a real bug from getting this wrong -- see hal_video.h's hal_video_run()
// doc comment): arduino-pico launches Core 1's setup1()/loop1()
// automatically and CONCURRENTLY with Core 0's setup(), unlike the
// original's manual multicore_launch_core1() call placed after asset
// loading. g_video_ready gates Core 1 so hal_video_run() still only starts
// once Core 0 is about to begin continuously feeding scanlines -- otherwise
// the DVI pipeline sits starved for the whole (possibly slow, SD-card-retry
// laden) asset-load window, which showed up as a spurious colored flash on
// boot in the original Pico SDK build.
#include <arcade_hal_video.h>
#include <arcade_video_geom.h>   // av_geom_toggle_on_edge() -- Button 1
#include <arcade_hal_input.h>
#include <invaders_machine.h>
#include <invaders_video.h>
#include <invaders_input.h>
#include <board_config_fruitjam.h>

static arcade_system   g_system;
static volatile bool   g_video_ready = false;
static bool            g_assets_ok   = false;
static uint16_t        g_error_color = 0;

void setup() {
    // Serial for the frame-budget heartbeat in loop(). Same placement as
    // lrescue_fruitjam.ino: before Core 1 starts the DVI pump is the one
    // safe place to block briefly, giving a connecting serial monitor time
    // to attach. Note that this board's USB may be wired to a PIO host
    // peripheral rather than a device link to a PC (see CLAUDE.md), in which
    // case nothing shows up on a monitor; harmless either way.
    Serial.begin(115200);
    delay(1500);

    // PicoDVI's 640x480 mode requires the system clock to equal the TMDS
    // bit clock (252 MHz) -- must happen before any other peripheral init.
    set_sys_clock_khz(252000, true);

    // Sets game-state defaults and calls hal_video_init() (struct/queue
    // setup only -- does not start the physical DVI signal, does not touch
    // storage).
    invaders_init(&g_system);

    // Storage/ROM/WAV loading -- blocking, can be slow (SD card retries).
    // Deliberately finishes before Core 1 is allowed to start the DVI pump
    // (see g_video_ready below).
    g_assets_ok = invaders_load_assets(&g_system, &g_error_color);

    // From this point on, loop() will continuously feed scanlines (either
    // error frames or game frames) on every call -- safe for Core 1 to
    // start the DVI pump now.
    g_video_ready = true;
}

void loop() {
    if (!g_assets_ok) {
        invaders_draw_error_frame(g_error_color);
        return; // Arduino calls loop() again immediately; queue stays fed.
    }

    // Board-specific button wiring lives here, not in ArcadeMachine_Invaders
    // -- this sketch is the one place that knows both which physical button
    // is which (board_config_fruitjam.h's HAL_BTN_* indices) AND what each
    // one means for this game.
    bool coin   = hal_input_read(HAL_BTN_COIN);
    bool start1 = hal_input_read(HAL_BTN_START1);
    bool start2 = hal_input_read(HAL_BTN_START2);
    bool left   = hal_input_read(HAL_BTN_LEFT);
    bool right  = hal_input_read(HAL_BTN_RIGHT);
    bool shoot  = hal_input_read(HAL_BTN_SHOOT);
    bool rotate = hal_input_read(HAL_BTN_ROTATE);
    bool mirror = hal_input_read(HAL_BTN_MIRROR);
    // Button 1: aspect correction on/off. Edge-detected inside
    // av_geom_toggle_on_edge(); the right setting depends on the monitor,
    // not the game, so it is a runtime toggle rather than a build flag.
    av_geom_toggle_on_edge(hal_input_read(HAL_BTN_STRETCH));

    invaders_input_update(&g_system, coin, start1, start2, left, right, shoot, rotate, mirror);

    // Frame-budget instrument -- the same one lrescue_fruitjam.ino and
    // galaga_fruitjam.ino carry. `frame` alone tells you nothing, because
    // hal_video_acquire_scanline() BLOCKS until Core 1's DVI pump frees a
    // buffer: the loop measures max(work, DVI frame period) and pins at
    // ~16.7ms as soon as the work fits. `work` (frame minus the blocked
    // time, via hal_video_take_blocked_us()) is the real Core 0 cost, and
    // DEVNOTES.md problem #16 dead-ended for sessions precisely because that
    // split did not exist yet.
    //
    // This game had never been measured at all before the problem #34
    // interleave was back-applied to it; this is here to give that number
    // and to confirm the interleave did not add per-frame cost (Lunar
    // Rescue's first attempt did, via an int64 divide in the 240-iteration
    // loop -- ~700us/frame). Safe to delete once that is on record in
    // DEVNOTES.md.
    static uint32_t frame_count = 0;
    static uint32_t work_max = 0;
    uint32_t t0 = micros();
    invaders_run_frame(&g_system);
    uint32_t frame_us   = micros() - t0;
    uint32_t blocked_us = hal_video_take_blocked_us();
    uint32_t work_us    = (frame_us > blocked_us) ? (frame_us - blocked_us) : 0;
    if (work_us > work_max) work_max = work_us;

    if ((++frame_count % 60u) == 0) {
        Serial.print("[invaders] frame ");
        Serial.print(frame_count);
        Serial.print(", frame ");
        Serial.print(frame_us);
        Serial.print("us (work ");
        Serial.print(work_us);
        Serial.print("us, blocked ");
        Serial.print(blocked_us);
        Serial.print("us), work_max ");
        Serial.print(work_max);
        Serial.println("us");
    }
}

void setup1() {
    while (!g_video_ready) {
        tight_loop_contents(); // spin until Core 0 is ready to feed continuously
    }
    hal_video_run(); // never returns
}

void loop1() {
    // Unreachable -- hal_video_run() in setup1() never returns.
}
