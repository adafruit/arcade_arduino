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
#include <arcade_hal_input.h>
#include <mspacman_machine.h>
#include <mspacman_video.h>
#include <mspacman_input.h>
#include <board_config_fruitjam.h>

static mspacman_system g_system;
static volatile bool   g_video_ready = false;
static bool            g_assets_ok   = false;
static uint16_t        g_error_color = 0;

void setup() {
    // PicoDVI's 640x480 mode requires the system clock to equal the TMDS
    // bit clock (252 MHz) -- must happen before any other peripheral init.
    set_sys_clock_khz(252000, true);

    // Sets game-state defaults, selects the aux board's decrypted ROM bank,
    // and calls hal_video_init() (struct/queue setup only -- does not start
    // the physical DVI signal, does not touch storage).
    mspacman_init(&g_system);

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

    mspacman_input_update(&g_system, coin, start1, start2, up, down, left, right, rotate, mirror);
    mspacman_run_frame(&g_system);
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
