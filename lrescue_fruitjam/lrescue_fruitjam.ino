// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// lrescue_fruitjam -- Lunar Rescue (ArcadeMachine_LunarRescue) on the
// Adafruit Fruit Jam (ArcadeBoard_FruitJam). Sibling sketch to
// invaders_fruitjam.ino -- see that file's header comment for the full
// rationale behind the SAMP composition-root pattern and the Core 0/Core 1
// boot-order constraint (g_video_ready gating hal_video_run() until Core 0
// is continuously feeding scanlines); both apply here unchanged, since
// Lunar Rescue is on the same "8080bw" board family as Space Invaders and
// this sketch reuses the same ArcadeBoard_FruitJam HAL backend.
//
// Core 0: game emulation, input polling, board-to-game input mapping.
// Core 1: hal_video_run() -- drives the DVI signal; never returns.
#include <arcade_hal_video.h>
#include <arcade_hal_input.h>
#include <lrescue_machine.h>
#include <lrescue_video.h>
#include <lrescue_input.h>
#include <lrescue_audio.h>
#include <board_config_fruitjam.h>

static arcade_system   g_system;
static volatile bool   g_video_ready = false;
static bool            g_assets_ok   = false;
static uint16_t        g_error_color = 0;

// DEBUG: hold COIN through boot to drop into a two-phase sound self-test
// instead of starting the game:
//   1. REPORT: one flash per sample slot, in filename order (alienexplosion,
//      rescueshipexplosion, beamgun, thrust, bonus2, bonus3, shootingstar,
//      stepl, steph), colored by lrescue_audio_sample_status():
//        GREEN  = loaded fine
//        RED    = hal_storage_open() failed (file missing/unreadable)
//        BLUE   = opened, but the WAV header didn't parse
//        YELLOW = header parsed fine, but conversion wrote 0 bytes
//      This board's USB is wired as a PIO host port, not a debug link to a
//      PC (see CLAUDE.md), so this is the only way to get a real
//      load-status readout without adding a UART console.
//   2. CYCLE: lrescue_audio_play() through all 9 slots in order (plus a
//      speaker-channel on/off toggle), one every ~1.5s, forever.
// Bypasses lrescue_ports.cpp's port-trigger mapping entirely -- isolates
// "didn't load" from "loaded fine, just rare/silent in actual gameplay".
// Safe to delete once sound is confirmed working end to end; independent
// of the normal game path below.
static bool g_self_test = false;

// DEBUG: an unmistakable magenta flash that runs exactly once, right at the
// start of the self-test, before anything else. If the board is actually
// crashing/resetting mid-test rather than just behaving oddly, this flash
// will reappear (interrupting whatever was happening) every time it
// reboots -- a much more reliable "did it actually reset?" signal than
// trying to infer that from audio/video behavior alone.
enum { SELFTEST_BEACON, SELFTEST_REPORT, SELFTEST_SIZECHECK, SELFTEST_CYCLE };
static int self_test_phase = SELFTEST_BEACON;

static void self_test_beacon_step() {
    static uint32_t frame = 0;
    const uint32_t BEACON_FRAMES = 20; // ~0.33s
    lrescue_draw_error_frame(0xF81F); // magenta -- doesn't appear anywhere else in this self-test
    if (++frame >= BEACON_FRAMES) { self_test_phase = SELFTEST_REPORT; frame = 0; }
}

static uint16_t status_color(lrescue_sample_t sample) {
    switch (lrescue_audio_sample_status(sample)) {
        case LRESCUE_AUDIO_STATUS_OK:             return 0x07E0; // green
        case LRESCUE_AUDIO_STATUS_OPEN_FAILED:     return 0xF800; // red
        case LRESCUE_AUDIO_STATUS_HEADER_FAILED:   return 0x001F; // blue
        case LRESCUE_AUDIO_STATUS_NO_DATA_WRITTEN: return 0xFFE0; // yellow
        default:                                   return 0xFFFF; // white -- shouldn't happen post-load
    }
}

static void self_test_report_step() {
    static uint32_t frame = 0;
    static int      slot  = 0;
    const uint32_t ON_FRAMES  = 30; // ~0.5s
    const uint32_t GAP_FRAMES = 20; // ~0.33s black between flashes

    if (slot >= LRESCUE_NUM_SAMPLES) {
        // All 9 reported -- pause on black, then move to the size check.
        lrescue_draw_error_frame(0x0000);
        if (++frame >= 60) { self_test_phase = SELFTEST_SIZECHECK; frame = 0; slot = 0; }
        return;
    }

    if (frame < ON_FRAMES) {
        lrescue_draw_error_frame(status_color((lrescue_sample_t)slot));
    } else {
        lrescue_draw_error_frame(0x0000);
    }
    if (++frame >= ON_FRAMES + GAP_FRAMES) { slot++; frame = 0; }
}

// DEBUG: for every slot that reported OK (whichever those are -- not
// hardcoded, since lrescue_audio_load_samples() now loads in size order,
// not filename order, as its own diagnostic experiment), flashes GREEN if
// lrescue_audio_sample_bytes() is a sane size or RED if it's suspiciously
// large (every one of the 9 real files converts to well under 60000 bytes
// if it stops at its own true length -- the biggest, bonus3.wav, is still
// only ~61778). Slots that didn't report OK get a quick, unmistakable
// black blink instead of a full-length slot, so this doesn't just repeat
// the REPORT phase's timing for no reason. Exists to check directly
// whether a sample's conversion ran away past its file's true length,
// whichever sample that turns out to be.
static void self_test_sizecheck_step() {
    static uint32_t frame = 0;
    static int      slot  = 0;
    const uint32_t ON_FRAMES  = 45; // ~0.75s -- a bit longer, this one matters
    const uint32_t GAP_FRAMES = 30;
    const uint32_t SKIP_FRAMES = 10; // for slots that aren't OK -- quick, not silent
    const uint32_t EXPECTED_MAX_BYTES = 65000;

    if (slot >= LRESCUE_NUM_SAMPLES) {
        lrescue_draw_error_frame(0x0000);
        if (++frame >= 60) { self_test_phase = SELFTEST_CYCLE; frame = 0; slot = 0; }
        return;
    }

    if (lrescue_audio_sample_status((lrescue_sample_t)slot) != LRESCUE_AUDIO_STATUS_OK) {
        lrescue_draw_error_frame(frame < SKIP_FRAMES / 2 ? 0xFFFF : 0x0000); // brief white blink, then black
        if (++frame >= SKIP_FRAMES) { slot++; frame = 0; }
        return;
    }

    if (frame < ON_FRAMES) {
        uint32_t bytes = lrescue_audio_sample_bytes((lrescue_sample_t)slot);
        lrescue_draw_error_frame(bytes > 0 && bytes < EXPECTED_MAX_BYTES ? 0x07E0 /* green */ : 0xF800 /* red */);
    } else {
        lrescue_draw_error_frame(0x0000);
    }
    if (++frame >= ON_FRAMES + GAP_FRAMES) { slot++; frame = 0; }
}

static void self_test_cycle_step() {
    // Steps 0..8 play each sample slot in order; step 9 turns the speaker
    // channel on for one step, step 10 turns it back off, then it repeats.
    // Unlike the original version of this test, the screen keeps flashing
    // during this phase too: the instant a step triggers, it shows that
    // slot's status color (same mapping as the REPORT phase) for the first
    // third of the step, so whatever you hear right then is tied to a color
    // in the same instant -- no need to remember the REPORT phase's order
    // from a minute earlier.
    static const int NUM_STEPS = LRESCUE_NUM_SAMPLES + 2;
    static uint32_t frame = 0;
    static int step = -1;
    const uint32_t FRAMES_PER_STEP = 90;   // ~1.5s at ~60Hz
    const uint32_t INDICATOR_FRAMES = 30;  // ~0.5s of color at the start of each step

    if (frame % FRAMES_PER_STEP == 0) {
        // shootingstar is the one sample that's supposed to loop until
        // something explicitly stops it (matching real gameplay, where
        // lrescue_ports.cpp stops it on a falling edge) -- this self-test
        // never generates that falling edge, so without this it would
        // loop forever underneath every subsequent step once triggered.
        if (step == LRESCUE_SND_SHOOTINGSTAR) lrescue_audio_stop(LRESCUE_SND_SHOOTINGSTAR);
        step = (step + 1) % NUM_STEPS;
        if (step < LRESCUE_NUM_SAMPLES) {
            lrescue_audio_play((lrescue_sample_t)step);
        } else {
            // g_system.total_cycles, matching lrescue_ports.cpp's real
            // port 5 write handler exactly (see that call's doc comment for
            // why total_cycles, not a real-time clock, is the correct
            // choice for a frame-batched CPU emulator). This self-test
            // never calls lrescue_run_frame(), so total_cycles never
            // advances here -- both this "on" and the later "off" event end
            // up timestamped at the same cycle, but the ring buffer
            // preserves the order they were pushed in regardless, so the
            // simple on/off toggle this test wants still works correctly.
            lrescue_audio_speaker_event(g_system.total_cycles, step == LRESCUE_NUM_SAMPLES);
        }
    }
    uint32_t frame_in_step = frame % FRAMES_PER_STEP;
    frame++;

    if (frame_in_step < INDICATOR_FRAMES) {
        uint16_t color = (step < LRESCUE_NUM_SAMPLES)
            ? status_color((lrescue_sample_t)step)
            : (step == LRESCUE_NUM_SAMPLES ? 0xFFFF /* speaker on */ : 0x0000 /* speaker off */);
        lrescue_draw_error_frame(color);
    } else {
        lrescue_draw_frame(&g_system); // keep Core 1's DVI pipeline fed -- see setup1()'s doc comment
    }
}

static void self_test_loop() {
    switch (self_test_phase) {
        case SELFTEST_BEACON:    self_test_beacon_step();    break;
        case SELFTEST_REPORT:    self_test_report_step();    break;
        case SELFTEST_SIZECHECK: self_test_sizecheck_step(); break;
        default:                 self_test_cycle_step();     break;
    }
}

void setup() {
    // DEBUG: real serial diagnostics for the WAV loader -- see
    // lrescue_audio.cpp. This board's USB may be wired to a PIO host
    // peripheral rather than a device link to a PC (see CLAUDE.md), in
    // which case nothing will show up on a serial monitor; harmless either
    // way. Safe here (before Core 1 starts the DVI pump) to block briefly
    // giving a connecting serial monitor time to attach.
    Serial.begin(115200);
    delay(1500);

    // PicoDVI's 640x480 mode requires the system clock to equal the TMDS
    // bit clock (252 MHz) -- must happen before any other peripheral init.
    set_sys_clock_khz(252000, true);

    lrescue_init(&g_system);

    // Boot straight into a chosen rotation, for measuring one orientation
    // without a hand on the rotate button:
    //   arduino-cli compile --build-property compiler.cpp.extra_flags=-DTEST_ROTATION=0
    // 0 = landscape, 1 = 90 CCW tate (default), 2 = 180, 3 = 90 CW tate.
#ifdef TEST_ROTATION
    g_system.rotation = (uint8_t)(TEST_ROTATION);
    Serial.print("[lrescue] TEST_ROTATION override -> ");
    Serial.println((int)g_system.rotation);
#endif

    g_assets_ok = lrescue_load_assets(&g_system, &g_error_color);

    // hal_input_init() has run by now (inside lrescue_load_assets()) --
    // safe to read a button here. See self_test_loop()'s doc comment.
    if (g_assets_ok) g_self_test = hal_input_read(HAL_BTN_COIN);

    // From this point on, loop() will continuously feed scanlines (either
    // error frames or game frames) on every call -- safe for Core 1 to
    // start the DVI pump now.
    g_video_ready = true;
}

void loop() {
    if (!g_assets_ok) {
        lrescue_draw_error_frame(g_error_color);
        return; // Arduino calls loop() again immediately; queue stays fed.
    }

    if (g_self_test) {
        self_test_loop();
        return;
    }

    // Board-specific button wiring lives here, not in ArcadeMachine_LunarRescue
    // -- this sketch is the one place that knows both which physical button
    // is which (board_config_fruitjam.h's HAL_BTN_* indices) AND what each
    // one means for this game. Lunar Rescue's cabinet is a 2-way joystick +
    // a single fire/action button (see lrescue_input.h) -- LEFT/RIGHT map
    // directly to the joystick and SHOOT is that one action button, the
    // same physical buttons invaders_fruitjam.ino uses for the same
    // purpose. ROTATE/MIRROR remain the display meta-controls, unrelated
    // to gameplay, same as in invaders_fruitjam.ino.
    bool coin   = hal_input_read(HAL_BTN_COIN);
    bool start1 = hal_input_read(HAL_BTN_START1);
    bool start2 = hal_input_read(HAL_BTN_START2);
    bool left   = hal_input_read(HAL_BTN_LEFT);
    bool right  = hal_input_read(HAL_BTN_RIGHT);
    bool shot   = hal_input_read(HAL_BTN_SHOOT);
    bool rotate = hal_input_read(HAL_BTN_ROTATE);
    bool mirror = hal_input_read(HAL_BTN_MIRROR);

    lrescue_input_update(&g_system, coin, start1, start2, left, right, shot, rotate, mirror);

    // Frame-budget instrument -- the same one galaga_fruitjam.ino carries,
    // and the reason it is here: DEVNOTES.md problem #16 investigated this
    // game's residual red lines without any way to measure real Core 0
    // headroom, and explicitly dead-ended on that. `frame` alone cannot
    // tell you anything, because hal_video_acquire_scanline() BLOCKS until
    // Core 1's DVI pump frees a buffer -- the loop measures
    // max(work, DVI frame period) and pins at ~16.7ms as soon as the work
    // fits. `work` (frame minus the blocked time, via
    // hal_video_take_blocked_us()) is the real cost.
    //
    // What to look for: red lines are PicoDVI's queue-starvation fallback
    // (#16). If `work` spikes toward or past 16670us during the bonus
    // arpeggio, Core 0 is genuinely overrunning and the cause is on this
    // side. If `work` stays comfortably under budget while red lines still
    // appear, that confirms #16's remaining hypothesis (DMA/bus contention
    // or a Core-1-side hiccup) and no amount of Core 0 optimisation will
    // help.
    static uint32_t frame_count = 0;
    static uint32_t work_max = 0;
    uint32_t t0 = micros();
    lrescue_run_frame(&g_system);
    uint32_t frame_us   = micros() - t0;
    uint32_t blocked_us = hal_video_take_blocked_us();
    uint32_t work_us    = (frame_us > blocked_us) ? (frame_us - blocked_us) : 0;
    if (work_us > work_max) work_max = work_us;

    if ((++frame_count % 60u) == 0) {
        Serial.print("[lrescue] frame ");
        Serial.print(frame_count);
        Serial.print(", frame ");
        Serial.print(frame_us);
        Serial.print("us (work ");
        Serial.print(work_us);
        Serial.print("us, blocked ");
        Serial.print(blocked_us);
        Serial.print("us), work_max ");
        Serial.print(work_max);
        // Queue-starvation counter -- the ONLY instrument that sees the red
        // (arcade_hal_video.h), and exactly what #16 above dead-ended for
        // want of.
        Serial.print("us, rot ");
        Serial.print((int)g_system.rotation);
        Serial.print(", starve ");
        Serial.print(hal_video_take_starve_count());
        Serial.println("/60");
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
