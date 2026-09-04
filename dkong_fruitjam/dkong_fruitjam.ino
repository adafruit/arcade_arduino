// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// dkong_fruitjam -- Donkey Kong (ArcadeMachine_DKong) on the Adafruit Fruit
// Jam (ArcadeBoard_FruitJam). This sketch is the SAMP composition root: it
// is the ONLY place that knows both "this game" and "this board" at once.
// invaders_fruitjam.ino and lrescue_fruitjam.ino document the full
// rationale behind this pattern and the Core 0/Core 1 boot-order constraint
// (g_video_ready gating hal_video_run() until Core 0 is continuously
// feeding scanlines); both apply here unchanged.
//
// SOUND IS NOT IMPLEMENTED for this game yet -- see dkong_audio.h. The
// audio hardware is still brought up and fed silence, so the DAC/I2S path
// is exercised on every boot and whoever adds the 8035 finds a working
// pipeline rather than an untested one.
//
// Core 0: game emulation, input polling, board-to-game input mapping.
// Core 1: hal_video_run() -- drives the DVI signal; never returns.
#include <arcade_hal_video.h>
#include <arcade_video_geom.h>
#include <arcade_hal_input.h>
#include <dkong_machine.h>
#include <dkong_video.h>
#include <dkong_input.h>
#include <dkong_assets.h>
#include <board_config_fruitjam.h>

static dkong_system    g_system;
static volatile bool   g_video_ready = false;
static bool            g_assets_ok   = false;
static uint16_t        g_error_color = 0;

void setup() {
    // Serial for the frame-budget heartbeat in loop(). Before Core 1 starts
    // the DVI pump is the one safe place to block briefly, giving a
    // connecting serial monitor time to attach.
    Serial.begin(115200);
    delay(1500);
    Serial.println("[dkong] boot: serial up");

    // PicoDVI's 640x480 mode requires the system clock to equal the TMDS
    // bit clock (252 MHz) -- must happen before any other peripheral init.
    set_sys_clock_khz(252000, true);
    Serial.println("[dkong] boot: sysclk set");

    // Sets game-state defaults and calls hal_video_init() (struct/queue
    // setup only -- does not start the physical DVI signal, does not touch
    // storage).
    dkong_init(&g_system);
    Serial.println("[dkong] boot: dkong_init done");

    // Boot straight into a chosen rotation, for measuring one orientation
    // without a hand on the rotate button:
    //   arduino-cli compile --build-property compiler.cpp.extra_flags=-DTEST_ROTATION=0
    // 0 = landscape, 1 = 90 CCW tate (default), 2 = 180, 3 = 90 CW tate.
#ifdef TEST_ROTATION
    g_system.rotation = (uint8_t)(TEST_ROTATION);
    Serial.print("[dkong] TEST_ROTATION override -> ");
    Serial.println((int)g_system.rotation);
#endif

    // Aspect-ratio correction (arcade_video_geom.h), same build-flag shape:
    //   --build-property compiler.cpp.extra_flags=-DTEST_STRETCH=1
    // This game is the project's tightest frame budget, so it is the one to
    // measure the correction's cost on.
#ifdef TEST_STRETCH
    av_geom_set_stretch(TEST_STRETCH != 0);
    Serial.print("[dkong] TEST_STRETCH -> ");
    Serial.println((int)av_geom_get_stretch());
#endif

    // Storage/ROM/PROM loading -- blocking, can be slow (SD card retries).
    // Deliberately finishes before Core 1 is allowed to start the DVI pump.
    Serial.println("[dkong] boot: loading assets...");
    g_assets_ok = dkong_load_assets(&g_system, &g_error_color);
    Serial.println("[dkong] boot: load_assets returned");

    // Boot result on serial. This exists because its absence cost a real
    // debugging cycle: the error path below draws a solid colour and
    // returns, printing NOTHING, so on the first flash of this game a
    // failed asset load was indistinguishable over the wire from a hang in
    // setup(). Every sketch in this project has that same gap; this one no
    // longer does. A boot-time print is free -- it happens once, before
    // Core 1 starts the DVI pump.
    if (g_assets_ok) {
        Serial.println("[dkong] assets loaded OK");
    } else {
        Serial.print("[dkong] ASSET LOAD FAILED, error color 0x");
        Serial.print(g_error_color, HEX);
        Serial.println(g_error_color == DKONG_COLOR_ERROR_NO_CARD
                       ? " (red: no SD card / would not mount)"
                       : " (yellow: card mounted, required ROM files missing)");
    }

    g_video_ready = true;
}

void loop() {
    if (!g_assets_ok) {
        // Report the failure ONCE PER SECOND, not once at boot. A boot-time
        // print is lost to a race that cost real time here: USB CDC
        // discards writes while no host is attached, and a host attaching
        // after `arduino-cli upload` returns is already seconds too late.
        // A periodic print is observable whenever someone looks, which is
        // the property that actually matters for a diagnostic.
        static uint32_t err_count = 0;
        if ((err_count++ % 60u) == 0) {
            Serial.print("[dkong] ASSET LOAD FAILED, error color 0x");
            Serial.print(g_error_color, HEX);
            Serial.println(g_error_color == DKONG_COLOR_ERROR_NO_CARD
                           ? " (red: no SD card / would not mount)"
                           : " (yellow: card mounted, required ROM files missing)");
            Serial.print("[dkong]   could not load: ");
            Serial.println(dkong_debug_missing_files());
        }
        dkong_draw_error_frame(g_error_color);
        return; // Arduino calls loop() again immediately; queue stays fed.
    }

    // Board-specific button wiring lives here, not in ArcadeMachine_DKong.
    // A Donkey Kong cabinet is a 4-way joystick plus ONE action button, and
    // that button is JUMP -- mapped to the board's existing HAL_BTN_SHOOT
    // (GPIO 10, header D10), the same physical button Space Invaders fires
    // with and Galaga shoots with. No new board wiring is needed for this
    // game.
    //
    // Note the machine library still calls its parameter `jump`, not
    // `shoot`: ArcadeMachine_DKong only knows game-semantic actions, and
    // which physical button produces one is exactly the decision this
    // sketch exists to make. That split is why adding this game touched no
    // board file at all.
    bool coin   = hal_input_read(HAL_BTN_COIN);
    bool start1 = hal_input_read(HAL_BTN_START1);
    bool start2 = hal_input_read(HAL_BTN_START2);
    bool up     = hal_input_read(HAL_BTN_UP);
    bool down   = hal_input_read(HAL_BTN_DOWN);
    bool left   = hal_input_read(HAL_BTN_LEFT);
    bool right  = hal_input_read(HAL_BTN_RIGHT);
    bool jump   = hal_input_read(HAL_BTN_SHOOT);
    bool rotate = hal_input_read(HAL_BTN_ROTATE);
    bool mirror = hal_input_read(HAL_BTN_MIRROR);
    // Button 1: aspect correction on/off. Edge-detected inside
    // av_geom_toggle_on_edge(); the right setting depends on the monitor,
    // not the game, so it is a runtime toggle rather than a build flag.
    const bool stretch_btn = hal_input_read(HAL_BTN_STRETCH);
    av_geom_toggle_on_edge(stretch_btn);

    // BUTTON WITNESS, same idea as btime_fruitjam.ino's: a STICKY record of
    // which meta buttons were seen down since the last heartbeat. Without
    // it, "nothing happened" cannot be told apart from "the board never saw
    // the press", and those need completely different fixes.
    static uint8_t meta_seen = 0;
    if (rotate)      meta_seen |= 0x01;
    if (mirror)      meta_seen |= 0x02;
    if (stretch_btn) meta_seen |= 0x04;

    dkong_input_update(&g_system, coin, start1, start2,
                       up, down, left, right, jump, rotate, mirror);

    // Frame-budget instrument -- the same one the other sketches carry.
    // `frame` alone means nothing, because hal_video_acquire_scanline()
    // BLOCKS until Core 1 frees a buffer: the loop measures
    // max(work, DVI frame period) and pins at ~16.7ms as soon as the work
    // fits. `work` is the real Core 0 cost. See DEVNOTES.md #16/#25.
    //
    // Worth watching on this game in particular: it is the first here to
    // run a DMA burst inside the frame (the 8257 moves ~384 bytes through
    // the CPU's own read/write path once per frame), and its renderer walks
    // every sprite for every scanline.
    static uint32_t frame_count = 0;
    // Totals beside every maximum -- see DEVNOTES #84/#85.
    static uint32_t work_max = 0, work_sum = 0, work_n = 0;
    static uint32_t blk_sum = 0, blk_max = 0;
    uint32_t t0 = micros();
    dkong_run_frame(&g_system);
    uint32_t frame_us   = micros() - t0;
    uint32_t blocked_us = hal_video_take_blocked_us();
    uint32_t work_us    = (frame_us > blocked_us) ? (frame_us - blocked_us) : 0;
    if (work_us > work_max) work_max = work_us;
    if (blocked_us > blk_max) blk_max = blocked_us;
    work_sum += work_us; blk_sum += blocked_us; work_n++;

    if ((++frame_count % 60u) == 0) {
        Serial.print("[dkong] frame ");
        Serial.print(frame_count);
        Serial.print(", frame ");
        Serial.print(frame_us);
        Serial.print("us (work ");
        Serial.print(work_us);
        Serial.print("us, blocked ");
        Serial.print(blocked_us);
        Serial.print("us), work_MEAN ");
        Serial.print(work_n ? work_sum / work_n : 0);
        Serial.print("us, work_max ");
        Serial.print(work_max);
        Serial.print("us, blk_MEAN ");
        Serial.print(work_n ? blk_sum / work_n : 0);
        Serial.print("us, audio ");
        Serial.print(dkong_debug_audio_us());
        // Queue-starvation counter -- the ONLY instrument that sees the red
        // (arcade_hal_video.h): `work` can sit inside the frame budget while
        // an uneven patch inside that frame drains Core 1's queue anyway.
        Serial.print("us, rot ");
        Serial.print((int)g_system.rotation);
        Serial.print(", stretch ");
        Serial.print((int)av_geom_get_stretch());
        Serial.print(", meta_seen(RMS) ");
        Serial.print(meta_seen, BIN);
        meta_seen = 0;
        Serial.print(", starve ");
        Serial.print(hal_video_take_starve_count());
        // Runway left at the worst moment -- see DEVNOTES #85.
        Serial.print(", minq ");
        Serial.print(hal_video_take_min_valid_level());
        Serial.print("/");
        Serial.print(hal_video_scanbuf_count());
        work_sum = blk_sum = work_n = 0; work_max = 0; blk_max = 0;
        // Where the render half of `work` actually goes -- see
        // dkong_video.h. `rows` below `lines` means the duplicate-row
        // memoisation is firing.
        uint32_t r_us, e_us, rows, lines;
        dkong_debug_take_render(&r_us, &e_us, &rows, &lines);
        Serial.print("/60, rows ");
        Serial.print(rows);
        Serial.print("/");
        Serial.print(lines);
        Serial.print(" (");
        Serial.print(r_us / 60u);
        Serial.print("us/f), emit ");
        Serial.print(e_us / 60u);
        Serial.println("us/f");
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
