// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// mspacman_fruitjam -- Ms. Pac-Man (ArcadeMachine_MsPacman) on the Adafruit
// Fruit Jam (ArcadeBoard_FruitJam). This sketch is the SAMP composition
// root: it is the ONLY place that knows both "this game" and "this board"
// at once. Sibling sketch to pacman_fruitjam.ino, which it is otherwise a
// copy of -- Ms. Pac-Man runs on Pac-Man's board with the same controls, so
// the board-to-game wiring below is identical. invaders_fruitjam.ino and
// lrescue_fruitjam.ino document the full rationale behind this pattern and
// the Core 0/Core 1 boot-order constraint (g_video_ready gating
// hal_video_run() until Core 0 is continuously feeding scanlines); both
// apply here unchanged.
//
// Core 0: game emulation, input polling, board-to-game input mapping.
// Core 1: hal_video_run() -- drives the DVI signal; never returns.
#include <arcade_hal_video.h>
#include <arcade_video_geom.h>
#include <arcade_hal_input.h>
#include <mspacman_machine.h>
#include <mspacman_video.h>
#include <mspacman_input.h>
#include <mspacman_assets.h>
#include <board_config_fruitjam.h>

static mspacman_system g_system;
static volatile bool   g_video_ready = false;
static bool            g_assets_ok   = false;
static uint16_t        g_error_color = 0;

void setup() {
    // Serial diagnostics. This sketch had NONE, which is why a red screen on
    // it could not be told apart from PicoDVI's own starvation-red without
    // reflashing -- see DEVNOTES.md #43/#49. Before Core 1 starts the DVI
    // pump is the one safe place to block briefly.
    Serial.begin(115200);
    delay(1500);

    // PicoDVI's 640x480 mode requires the system clock to equal the TMDS
    // bit clock (252 MHz) -- must happen before any other peripheral init.
    set_sys_clock_khz(252000, true);

    // Sets game-state defaults, selects the aux board's decrypted ROM bank,
    // and calls hal_video_init() (struct/queue setup only -- does not start
    // the physical DVI signal, does not touch storage).
    mspacman_init(&g_system);

    // Boot straight into a chosen rotation, for measuring one orientation
    // without a hand on the rotate button:
    //   arduino-cli compile --build-property compiler.cpp.extra_flags=-DTEST_ROTATION=2
    // 0 = landscape, 1 = 90 CCW tate, 2 = 180, 3 = 90 CW tate (this game's
    // default). NOTE rotation 2 is the UPRIGHT landscape for this family and
    // 0 is upside-down, matching the tate default being 3 rather than 1 --
    // see DEVNOTES #33.
#ifdef TEST_ROTATION
    g_system.rotation = (uint8_t)(TEST_ROTATION);
#endif
    // Aspect-ratio correction, which mspacman_init() turns on by default:
    //   --build-property compiler.cpp.extra_flags=-DTEST_STRETCH=0
    // forces it off for an A/B against the historical 1:1 layout.
#ifdef TEST_STRETCH
    av_geom_set_stretch(TEST_STRETCH != 0);
#endif

    // Storage/ROM/PROM loading -- blocking, can be slow (SD card retries).
    // For this game it also builds the aux board's decrypted ROM bank out of
    // u5/u6/u7 (see mspacman_assets.cpp), which is pure CPU work on already-
    // loaded data and adds no meaningful time to the SD phase.
    // Deliberately finishes before Core 1 is allowed to start the DVI pump
    // (see g_video_ready below).
    g_assets_ok = mspacman_load_assets(&g_system, &g_error_color);

    // From this point on, loop() will continuously feed scanlines (either
    // error frames or game frames) on every call -- safe for Core 1 to
    // start the DVI pump now.
    g_video_ready = true;
}

void loop() {
    if (!g_assets_ok) {
        // Reported ONCE PER SECOND rather than once at boot: USB CDC
        // discards writes while no host is attached, so a boot-time print is
        // lost to any workflow that flashes and then opens the port.
        static uint32_t err_count = 0;
        if ((err_count++ % 60u) == 0) {
            Serial.print("[mspacman] ASSET LOAD FAILED, error color 0x");
            Serial.print(g_error_color, HEX);
            Serial.println(g_error_color == MSPACMAN_COLOR_ERROR_NO_CARD
                           ? " (red: no SD card / would not mount)"
                           : " (yellow: card mounted, required ROM files missing)");
            Serial.print("[mspacman]   could not load: ");
            Serial.println(mspacman_debug_missing_files());
        }
        mspacman_draw_error_frame(g_error_color);
        return; // Arduino calls loop() again immediately; queue stays fed.
    }

    // Board-specific button wiring lives here, not in
    // ArcadeMachine_MsPacman -- this sketch is the one place that knows both
    // which physical button is which (board_config_fruitjam.h's HAL_BTN_*
    // indices) AND what each one means for this game. Identical to
    // pacman_fruitjam.ino's: a Ms. Pac-Man cabinet is the same 4-way
    // joystick + coin/start, with no action button, so HAL_BTN_SHOOT is
    // unused here too.
    bool coin   = hal_input_read(HAL_BTN_COIN);
    bool start1 = hal_input_read(HAL_BTN_START1);
    bool start2 = hal_input_read(HAL_BTN_START2);
    bool up     = hal_input_read(HAL_BTN_UP);
    bool down   = hal_input_read(HAL_BTN_DOWN);
    bool left   = hal_input_read(HAL_BTN_LEFT);
    bool right  = hal_input_read(HAL_BTN_RIGHT);
    bool rotate = hal_input_read(HAL_BTN_ROTATE);
    bool mirror = hal_input_read(HAL_BTN_MIRROR);

    // Scripted play, opt-in: -DTEST_AUTOSTART=1. Same rationale as the
    // Pac-Man sketch (DEVNOTES #86): attract mode is a lower bound, and the
    // budget has to be measured with a coin in.
#ifdef TEST_AUTOSTART
    {
        static uint32_t autof = 0;
        autof++;
        const uint32_t f = autof % 6000u;
        if (f > 600u && f < 660u)  coin   = true;
        if (f > 780u && f < 840u)  start1 = true;
        if (f > 1000u) {
            const uint32_t d = (f / 90u) & 3u;
            left  = (d == 0); up = (d == 1); right = (d == 2); down = (d == 3);
        }
    }
#endif

    mspacman_input_update(&g_system, coin, start1, start2, up, down, left, right, rotate, mirror);

    // Frame-budget heartbeat, and proof of life: if this prints, the game is
    // running and any red on screen is DVI starvation, not an asset failure.
    static uint32_t frame_count = 0;
    // Totals beside every maximum -- see DEVNOTES #84/#85.
    static uint32_t work_max = 0, work_sum = 0, work_n = 0;
    static uint32_t blk_sum = 0, blk_max = 0;
    uint32_t t0 = micros();
    mspacman_run_frame(&g_system);
    uint32_t frame_us   = micros() - t0;
    uint32_t blocked_us = hal_video_take_blocked_us();
    uint32_t work_us    = (frame_us > blocked_us) ? (frame_us - blocked_us) : 0;
    if (work_us > work_max) work_max = work_us;
    if (blocked_us > blk_max) blk_max = blocked_us;
    work_sum += work_us; blk_sum += blocked_us; work_n++;

    if ((++frame_count % 60u) == 0) {
        Serial.print("[mspacman] frame ");
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
        Serial.print("us, blk_MAX ");
        Serial.print(blk_max);
        // rot/stretch/starve: "is the red gone" has to be a number per
        // rotation, not an impression (DEVNOTES #35/#79).
        Serial.print("us, rot ");
        Serial.print((int)g_system.rotation);
        Serial.print(", stretch ");
        Serial.print((int)av_geom_get_stretch());
        Serial.print(", starve ");
        Serial.print(hal_video_take_starve_count());
        // Runway left at the worst moment -- meaningful at any queue depth,
        // unlike a starve threshold. See DEVNOTES #85.
        Serial.print(", minq ");
        Serial.print(hal_video_take_min_valid_level());
        Serial.print("/");
        Serial.print(hal_video_scanbuf_count());
        Serial.println(" /60");
        work_sum = blk_sum = work_n = 0; work_max = 0; blk_max = 0;
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
