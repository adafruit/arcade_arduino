// galaga_fruitjam -- Galaga (ArcadeMachine_Galaga) on the Adafruit Fruit
// Jam (ArcadeBoard_FruitJam). This sketch is the SAMP composition root: it
// is the ONLY place that knows both "this game" and "this board" at once.
// Sibling sketch to invaders_fruitjam.ino/lrescue_fruitjam.ino/
// pacman_fruitjam.ino, which document the full rationale behind this
// pattern and the Core 0/Core 1 boot-order constraint (g_video_ready
// gating hal_video_run() until Core 0 is continuously feeding scanlines);
// both apply here unchanged.
//
// Working: all 3 Z80 CPUs, all three video layers (05XX starfield, sprites,
// tilemap -- see galaga_video.cpp), joystick/fire/coin/start input, Namco
// WSG audio (shots, dives, the attract theme) and the 54XX explosion
// channel (see galaga_54xx.h for what in it is cited and what is by ear).
//
// Core 0: game emulation, input polling, board-to-game input mapping.
// Core 1: hal_video_run() -- drives the DVI signal; never returns.
#include <arcade_hal_video.h>
#include <arcade_hal_input.h>
#include <galaga_machine.h>
#include <galaga_video.h>
#include <galaga_input.h>
#include <board_config_fruitjam.h>

static galaga_system    g_system;
static volatile bool    g_video_ready = false;
static bool             g_assets_ok   = false;
static uint16_t         g_error_color = 0;

void setup() {
    // DEBUG: boot-stage tracing to isolate the red-screen (NO_CARD)
    // report -- pull this out once the cause is found and noted in
    // DEVNOTES.md. Delay gives the Serial Monitor/CDC host time to attach
    // before the first print, same convention sd_test_fruitjam.ino uses.
    Serial.begin(115200);
    delay(2000);
    Serial.println("[galaga] setup() start");

    // PicoDVI's 640x480 mode requires the system clock to equal the TMDS
    // bit clock (252 MHz) -- must happen before any other peripheral init.
    set_sys_clock_khz(252000, true);
    Serial.println("[galaga] sys clock set to 252MHz");

    // Sets game-state defaults, wires all 3 Z80 cores, and calls
    // hal_video_init() (struct/queue setup only -- does not start the
    // physical DVI signal, does not touch storage).
    galaga_init(&g_system);
    Serial.println("[galaga] galaga_init() done (hal_video_init() called)");

    // Storage/ROM/PROM loading -- blocking, can be slow (SD card
    // retries). Deliberately finishes before Core 1 is allowed to start
    // the DVI pump (see g_video_ready below).
    Serial.println("[galaga] calling galaga_load_assets()...");
    g_assets_ok = galaga_load_assets(&g_system, &g_error_color);
    Serial.print("[galaga] galaga_load_assets() returned ");
    Serial.print(g_assets_ok ? "true" : "false");
    if (!g_assets_ok) {
        Serial.print(" (error_color=0x");
        Serial.print(g_error_color, HEX);
        Serial.print(", ");
        Serial.print(g_error_color == GALAGA_COLOR_ERROR_NO_CARD ? "NO_CARD" : "NO_ASSETS");
        Serial.print(")");
    }
    Serial.println();

    // From this point on, loop() will continuously feed scanlines (either
    // error frames or game frames) on every call -- safe for Core 1 to
    // start the DVI pump now.
    g_video_ready = true;
    Serial.println("[galaga] setup() done, g_video_ready = true");
}

void loop() {
    if (!g_assets_ok) {
        galaga_draw_error_frame(g_error_color);
        return; // Arduino calls loop() again immediately; queue stays fed.
    }

    // Board-specific button wiring lives here, not in ArcadeMachine_Galaga
    // -- this sketch is the one place that knows both which physical
    // button is which (board_config_fruitjam.h's HAL_BTN_* indices) AND
    // what each one means for this game. Galaga's cabinet is a 2-way
    // (left/right only) joystick + 1 fire button + coin/start -- the same
    // physical buttons Space Invaders' cabinet uses, no new board buttons
    // needed (unlike Pac-Man, which added UP/DOWN for its 4-way stick).
    bool coin   = hal_input_read(HAL_BTN_COIN);
    bool start1 = hal_input_read(HAL_BTN_START1);
    bool start2 = hal_input_read(HAL_BTN_START2);
    bool left   = hal_input_read(HAL_BTN_LEFT);
    bool right  = hal_input_read(HAL_BTN_RIGHT);
    bool fire   = hal_input_read(HAL_BTN_SHOOT);
    bool rotate = hal_input_read(HAL_BTN_ROTATE);
    bool mirror = hal_input_read(HAL_BTN_MIRROR);

    galaga_input_update(&g_system, coin, start1, start2, left, right, fire, rotate, mirror);

    // Frame-budget instrument. NOT scratch debugging -- keep it.
    //
    // `frame` alone is misleading: hal_video_acquire_scanline() BLOCKS
    // until Core 1's DVI pump frees a buffer, so this loop measures
    // max(work, DVI frame period) and pins at ~16.7ms as soon as the work
    // fits, hiding how much headroom is left. `work` (frame minus the time
    // spent blocked, via hal_video_take_blocked_us()) is the real cost and
    // the number to watch before adding anything to a frame.
    //
    // For reference, measured on hardware with input + WSG + 54XX audio:
    // ~8ms during boot (main CPU only), ~12.3ms once sub/sub2 come out of
    // reset, and ~14.4ms at gameplay peak against a 16.67ms budget. If
    // `work` approaches the budget the DVI queue starves and the display
    // shows partial or full red -- see hal_video_fruitjam.cpp.
    static uint32_t frame_count = 0;
    static uint32_t loop_start_ms = 0;
    if (frame_count == 0) {
        Serial.println("[galaga] loop(): first galaga_run_frame() call starting...");
        loop_start_ms = millis();
    }
    uint32_t t0 = micros();
    galaga_run_frame(&g_system);
    uint32_t t1 = micros();
    // Time actually spent computing this frame, i.e. excluding the wait for
    // Core 1's DVI pump to hand back a scanline buffer. This is the number
    // that shows real headroom -- see arcade_hal_video.h's
    // hal_video_take_blocked_us().
    uint32_t blocked_us = hal_video_take_blocked_us();
    uint32_t frame_us   = t1 - t0;
    uint32_t work_us    = (frame_us > blocked_us) ? (frame_us - blocked_us) : 0;

    if (frame_count == 0) {
        Serial.print("[galaga] loop(): first galaga_run_frame() took ");
        Serial.print(frame_us);
        Serial.println(" us");
    }
    frame_count++;

    // The one-shot palette/VRAM/sprite-RAM dump (frames 120/300) and the
    // ram2 write-trace dump (frame 900) that used to live here have been
    // removed: both were instrumentation for the 3-CPU boot deadlock,
    // which is solved (a DIP-switch bug -- see galaga_assets.cpp's dswa
    // comment). They also each blocked the frame loop for long enough to
    // starve the DVI scanline queue, which on this board shows as a
    // corrupted/solid-colour screen rather than a dropped frame. The
    // per-frame timing heartbeat below is kept (cheap, and the thing that
    // actually diagnosed the frame-budget overrun).

    if (frame_count % 60 == 0) {
        Serial.print("[galaga] loop(): frame ");
        Serial.print(frame_count);
        Serial.print(", frame ");
        Serial.print(frame_us / 1000);
        Serial.print("ms (work ");
        Serial.print(work_us);
        Serial.print("us, blocked ");
        Serial.print(blocked_us);
        Serial.print("us), ");
        Serial.print(frame_count * 1000UL / (millis() - loop_start_ms + 1));
        Serial.print(" fps avg since first frame, checksum pass=");
        Serial.print(g_system.debug_checksum_pass);
        Serial.print(" fail=");
        Serial.print(g_system.debug_checksum_fail);
        Serial.print(", main pc=0x");
        Serial.print(g_system.cpu_main.pc, HEX);
        Serial.print(" sp=0x");
        Serial.print(g_system.cpu_main.sp, HEX);
        // DEBUG: sub/sub2 PC (only ever dumped once before, at frame
        // 120/300 -- getting FRESH values here since those may be stale)
        // plus the exact handshake bytes 0x9100/0x9101 (ram2 offsets
        // 0x100/0x101) both main and sub/sub2 are spin-waiting on.
        Serial.print(", sub pc=0x");
        Serial.print(g_system.cpu_sub.pc, HEX);
        Serial.print(", sub2 pc=0x");
        Serial.print(g_system.cpu_sub2.pc, HEX);
        Serial.print(", ram2[0x100]=0x");
        Serial.print(g_system.ram2[0x100], HEX);
        Serial.print(" ram2[0x101]=0x");
        Serial.println(g_system.ram2[0x101], HEX);
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
