// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// pacman_fruitjam -- Pac-Man (ArcadeMachine_Pacman) on the Adafruit Fruit
// Jam (ArcadeBoard_FruitJam). This sketch is the SAMP composition root: it
// is the ONLY place that knows both "this game" and "this board" at once.
// The project's first Z80-based game -- sibling sketch to
// invaders_fruitjam.ino/lrescue_fruitjam.ino, which document the full
// rationale behind this pattern and the Core 0/Core 1 boot-order
// constraint (g_video_ready gating hal_video_run() until Core 0 is
// continuously feeding scanlines); both apply here unchanged.
//
// Core 0: game emulation, input polling, board-to-game input mapping.
// Core 1: hal_video_run() -- drives the DVI signal; never returns.
#include <arcade_hal_video.h>
#include <arcade_video_geom.h>
#include <arcade_hal_input.h>
#include <pacman_machine.h>
#include <pacman_video.h>
#include <pacman_input.h>
#include <board_config_fruitjam.h>

static pacman_system   g_system;
static volatile bool   g_video_ready = false;
static bool            g_assets_ok   = false;
static uint16_t        g_error_color = 0;

void setup() {
    // Serial for the frame-budget heartbeat in loop(). Before Core 1 starts
    // the DVI pump is the one safe place to block briefly, giving a
    // connecting serial monitor time to attach. This sketch carried no
    // serial at all until landscape/180 needed verifying per rotation.
    Serial.begin(115200);
    delay(1500);
    Serial.println("[pacman] boot: serial up");

    // PicoDVI's 640x480 mode requires the system clock to equal the TMDS
    // bit clock (252 MHz) -- must happen before any other peripheral init.
    set_sys_clock_khz(252000, true);

    // Sets game-state defaults and calls hal_video_init() (struct/queue
    // setup only -- does not start the physical DVI signal, does not touch
    // storage).
    pacman_init(&g_system);

    // Boot straight into a chosen rotation, for measuring one orientation
    // without a hand on the rotate button:
    //   arduino-cli compile --build-property compiler.cpp.extra_flags=-DTEST_ROTATION=0
    // 0 = landscape, 1 = 90 CCW tate, 2 = 180, 3 = 90 CW tate (this game's
    // default -- see pacman_init()).
#ifdef TEST_ROTATION
    g_system.rotation = (uint8_t)(TEST_ROTATION);
#endif
    // Aspect-ratio correction (arcade_video_geom.h), same build-flag shape:
    //   --build-property compiler.cpp.extra_flags=-DTEST_STRETCH=1
#ifdef TEST_STRETCH
    av_geom_set_stretch(TEST_STRETCH != 0);
#endif

    // Storage/ROM/PROM loading -- blocking, can be slow (SD card retries).
    // Deliberately finishes before Core 1 is allowed to start the DVI pump
    // (see g_video_ready below).
    g_assets_ok = pacman_load_assets(&g_system, &g_error_color);

    // From this point on, loop() will continuously feed scanlines (either
    // error frames or game frames) on every call -- safe for Core 1 to
    // start the DVI pump now.
    g_video_ready = true;
}

void loop() {
    if (!g_assets_ok) {
        pacman_draw_error_frame(g_error_color);
        return; // Arduino calls loop() again immediately; queue stays fed.
    }

    // Board-specific button wiring lives here, not in ArcadeMachine_Pacman
    // -- this sketch is the one place that knows both which physical
    // button is which (board_config_fruitjam.h's HAL_BTN_* indices) AND
    // what each one means for this game. Pac-Man's cabinet is a 4-way
    // joystick + coin/start (no action button) -- UP/DOWN are new buttons
    // added to ArcadeBoard_FruitJam specifically for this port (see
    // board_config_fruitjam.h); LEFT/RIGHT/COIN/START1/START2/ROTATE/
    // MIRROR are the same physical buttons the other two games use.
    // HAL_BTN_SHOOT is unused by this game.
    bool coin   = hal_input_read(HAL_BTN_COIN);
    bool start1 = hal_input_read(HAL_BTN_START1);
    bool start2 = hal_input_read(HAL_BTN_START2);
    bool up     = hal_input_read(HAL_BTN_UP);
    bool down   = hal_input_read(HAL_BTN_DOWN);
    bool left   = hal_input_read(HAL_BTN_LEFT);
    bool right  = hal_input_read(HAL_BTN_RIGHT);
    bool rotate = hal_input_read(HAL_BTN_ROTATE);
    bool mirror = hal_input_read(HAL_BTN_MIRROR);

    // Scripted play, opt-in:
    //   --build-property compiler.cpp.extra_flags=-DTEST_AUTOSTART=1
    // Attract mode understates the frame budget 2-3x -- every measurement
    // this project took from attract is a lower bound (DEVNOTES #82,
    // DISPLAY_GEOMETRY.md "Measure in gameplay, not in attract"). This puts
    // a coin in and walks Pac-Man around so the numbers are gameplay
    // numbers. The cycle repeats so a game over re-enters.
#ifdef TEST_AUTOSTART
    {
        static uint32_t autof = 0;
        autof++;
        const uint32_t f = autof % 6000u;
        if (f > 600u && f < 660u)  coin   = true;
        if (f > 780u && f < 840u)  start1 = true;
        if (f > 1000u) {
            // Rotate through the four directions so the maze actually gets
            // traversed and ghosts leave the house and chase.
            const uint32_t d = (f / 90u) & 3u;
            left  = (d == 0); up = (d == 1); right = (d == 2); down = (d == 3);
        }
    }
#endif

    pacman_input_update(&g_system, coin, start1, start2, up, down, left, right, rotate, mirror);

    // Frame-budget heartbeat, matching the other sketches. `frame` alone
    // means nothing because hal_video_acquire_scanline() BLOCKS until Core 1
    // frees a buffer -- the loop measures max(work, DVI frame period) and
    // pins at ~16.7ms as soon as the work fits. `work` is the real Core 0
    // cost, and `starve` is the only thing that sees the red (DEVNOTES #35).
    //
    // This game gained the heartbeat when landscape/180 stopped using a
    // whole-frame burst (DISPLAY_GEOMETRY.md phase 3): "is the red gone" is
    // a number, not an impression, and it has to be checked per rotation.
    // Totals beside every maximum -- a bare max is not a cost, and mixing a
    // window max with a single-frame sample is what hid Galaga's real
    // problem through five failed optimisations (DEVNOTES #84/#85).
    static uint32_t frame_count = 0;
    static uint32_t work_max = 0, work_sum = 0, work_n = 0;
    static uint32_t blk_sum = 0, blk_max = 0;
    uint32_t t0 = micros();
    pacman_run_frame(&g_system);
    uint32_t frame_us   = micros() - t0;
    uint32_t blocked_us = hal_video_take_blocked_us();
    uint32_t work_us    = (frame_us > blocked_us) ? (frame_us - blocked_us) : 0;
    if (work_us > work_max) work_max = work_us;
    if (blocked_us > blk_max) blk_max = blocked_us;
    work_sum += work_us; blk_sum += blocked_us; work_n++;

    if ((++frame_count % 60u) == 0) {
        Serial.print("[pacman] frame ");
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
        Serial.print("us, rot ");
        Serial.print((int)g_system.rotation);
        Serial.print(", stretch ");
        Serial.print((int)av_geom_get_stretch());
        Serial.print(", starve ");
        Serial.print(hal_video_take_starve_count());
        // Runway actually left at the worst moment -- meaningful at any
        // queue depth, unlike a starve threshold. See DEVNOTES #85.
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
