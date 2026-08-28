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
    // PicoDVI's 640x480 mode requires the system clock to equal the TMDS
    // bit clock (252 MHz) -- must happen before any other peripheral init.
    set_sys_clock_khz(252000, true);

    // Sets game-state defaults and calls hal_video_init() (struct/queue
    // setup only -- does not start the physical DVI signal, does not touch
    // storage).
    pacman_init(&g_system);

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

    pacman_input_update(&g_system, coin, start1, start2, up, down, left, right, rotate, mirror);
    pacman_run_frame(&g_system);
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
