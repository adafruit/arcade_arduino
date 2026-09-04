// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// btime_fruitjam -- Burger Time (ArcadeMachine_BTime) on the Adafruit Fruit
// Jam (ArcadeBoard_FruitJam). This sketch is the SAMP composition root: the
// ONLY place that knows both "this game" and "this board" at once.
// invaders_fruitjam.ino and lrescue_fruitjam.ino document the full
// rationale behind the pattern and the Core 0/Core 1 boot-order constraint
// (g_video_ready gating hal_video_run() until Core 0 is continuously
// feeding scanlines); both apply here unchanged.
//
// SOUND IS IMPLEMENTED: two emulated AY-3-8910s plus the board's discrete
// network (see btime_audio.h for what is modelled and the one part
// deliberately left out). Synthesis runs on Core 0 in slices inside the
// scanline loop and the audio ISR only copies out of a ring buffer, which
// is the split DEVNOTES.md #48 arrived at. Watch `underrun` in the
// heartbeat: nonzero means Core 0 is not keeping the ring fed, which is a
// frame-budget symptom rather than a synthesis one.
//
// Core 0: game emulation, input polling, board-to-game input mapping.
// Core 1: hal_video_run() -- drives the DVI signal; never returns.
#include <arcade_hal_video.h>
#include <arcade_video_geom.h>
#include <arcade_hal_input.h>
#include <btime_machine.h>
#include <btime_video.h>
#include <btime_input.h>
#include <btime_assets.h>
#include <btime_audio.h>
#include <board_config_fruitjam.h>

static btime_system    g_system;
static volatile bool   g_video_ready = false;
static bool            g_assets_ok   = false;
static uint16_t        g_error_color = 0;

void setup() {
    // Serial for the frame-budget heartbeat in loop(). Before Core 1 starts
    // the DVI pump is the one safe place to block briefly, giving a
    // connecting serial monitor time to attach.
    Serial.begin(115200);
    delay(1500);
    Serial.println("[btime] boot: serial up");

    // PicoDVI's 640x480 mode requires the system clock to equal the TMDS
    // bit clock (252 MHz) -- must happen before any other peripheral init.
    set_sys_clock_khz(252000, true);
    Serial.println("[btime] boot: sysclk set");

    // Sets game-state defaults, wires both 6502 cores (including the DECO
    // CPU-7's opcode hook), and calls hal_video_init() -- struct/queue
    // setup only. Does not start the physical DVI signal, does not touch
    // storage.
    btime_init(&g_system);

    // Boot straight into a chosen rotation, for measuring one orientation
    // without a hand on the rotate button:
    //   arduino-cli compile --build-property compiler.cpp.extra_flags=-DTEST_ROTATION=0
    // 0 = landscape, 1 = 90 CCW tate (this game's default), 2 = 180,
    // 3 = 90 CW tate.
#ifdef TEST_ROTATION
    g_system.rotation = (uint8_t)(TEST_ROTATION);
#endif
    // btime_init() turns the aspect correction on; -DTEST_STRETCH=0 forces
    // it off for an A/B against the historical 1:1 layout.
#ifdef TEST_STRETCH
    av_geom_set_stretch(TEST_STRETCH != 0);
#endif
    Serial.println("[btime] boot: btime_init done");

    // Storage/ROM loading -- blocking, can be slow (SD card retries).
    // Deliberately finishes before Core 1 is allowed to start the DVI pump.
    // This is also where both CPUs are reset, because a 6502 takes its
    // initial PC from 0xFFFC and that vector is in the ROM being loaded.
    Serial.println("[btime] boot: loading assets...");
    g_assets_ok = btime_load_assets(&g_system, &g_error_color);
    Serial.println("[btime] boot: load_assets returned");

    if (g_assets_ok) {
        Serial.println("[btime] assets loaded OK");
        // Not fatal, but worth knowing: a missing graphics ROM decodes to
        // blank characters, sprites or background rather than refusing to
        // boot, which is a silent visual degradation unless it is named.
        if (btime_debug_missing_files()[0]) {
            Serial.print("[btime] NOTE optional files missing: ");
            Serial.println(btime_debug_missing_files());
        }
    } else {
        Serial.print("[btime] ASSET LOAD FAILED, error color 0x");
        Serial.print(g_error_color, HEX);
        Serial.println(g_error_color == BTIME_COLOR_ERROR_NO_CARD
                       ? " (red: no SD card / would not mount)"
                       : " (yellow: card mounted, required ROM files missing)");
    }

    g_video_ready = true;
}

void loop() {
    if (!g_assets_ok) {
        // Report the failure ONCE PER SECOND, not once at boot. A boot-time
        // print is lost to a race that cost real time on Donkey Kong
        // (DEVNOTES.md #43): USB CDC discards writes while no host is
        // attached, and a host attaching after `arduino-cli upload` returns
        // is already seconds too late. A periodic print is observable
        // whenever someone looks, which is the property that matters for a
        // diagnostic.
        static uint32_t err_count = 0;
        if ((err_count++ % 60u) == 0) {
            Serial.print("[btime] ASSET LOAD FAILED, error color 0x");
            Serial.print(g_error_color, HEX);
            Serial.println(g_error_color == BTIME_COLOR_ERROR_NO_CARD
                           ? " (red: no SD card / would not mount)"
                           : " (yellow: card mounted, required ROM files missing)");
            Serial.print("[btime]   could not load: ");
            Serial.println(btime_debug_missing_files());
        }
        btime_draw_error_frame(g_error_color);
        return; // Arduino calls loop() again immediately; queue stays fed.
    }

    // Board-specific button wiring lives here, not in ArcadeMachine_BTime.
    // A Burger Time cabinet is a 4-way joystick plus ONE action button, and
    // that button throws pepper -- mapped to the board's existing
    // HAL_BTN_SHOOT (GPIO 10, header D10), the same physical button Space
    // Invaders fires with and Donkey Kong jumps with. No new board wiring
    // is needed for this game.
    //
    // The machine library calls its parameter `pepper`, not `shoot`:
    // ArcadeMachine_BTime only knows game-semantic actions, and which
    // physical button produces one is exactly the decision this sketch
    // exists to make.
    bool coin   = hal_input_read(HAL_BTN_COIN);
    bool start1 = hal_input_read(HAL_BTN_START1);
    bool start2 = hal_input_read(HAL_BTN_START2);
    bool up     = hal_input_read(HAL_BTN_UP);
    bool down   = hal_input_read(HAL_BTN_DOWN);
    bool left   = hal_input_read(HAL_BTN_LEFT);
    bool right  = hal_input_read(HAL_BTN_RIGHT);
    bool pepper = hal_input_read(HAL_BTN_SHOOT);
    bool rotate = hal_input_read(HAL_BTN_ROTATE);
    bool mirror = hal_input_read(HAL_BTN_MIRROR);
    // Button 1: aspect correction on/off. Edge-detected inside
    // av_geom_toggle_on_edge(); the right setting depends on the monitor,
    // not the game, so it is a runtime toggle rather than a build flag.
    av_geom_toggle_on_edge(hal_input_read(HAL_BTN_STRETCH));

    // Scripted play, opt-in: -DTEST_AUTOSTART=1. Same rationale as #86/#87.
    // Burger Time matters most here: it is the tightest game in the project
    // and every number it has ever produced came from attract mode, where
    // the demo looks so much like real play that it was mistaken for it once
    // already (DEVNOTES #81). Chef walks and throws pepper so the enemies
    // actually spawn and chase.
#ifdef TEST_AUTOSTART
    {
        static uint32_t autof = 0;
        autof++;
        const uint32_t f = autof % 6000u;
        if (f > 600u && f < 660u)  coin   = true;
        if (f > 780u && f < 840u)  start1 = true;
        if (f > 1000u) {
            const uint32_t d = (f / 75u) & 3u;
            left  = (d == 0); up = (d == 1); right = (d == 2); down = (d == 3);
            pepper = ((f / 40u) & 7u) == 0;  // occasional pepper cloud
        }
    }
#endif

    btime_input_update(&g_system, coin, start1, start2,
                       up, down, left, right, pepper, rotate, mirror);

    // BUTTON WITNESS. Reported in the heartbeat so that "the board does not
    // see this combination" and "the game declines this combination" can be
    // told apart -- they look identical from the player's side, and the
    // emulation accepts every combination in the host harness. `now` is
    // this frame's state; `seen` is the OR over the whole heartbeat
    // interval, so a tap that falls between two prints still shows up.
    const uint16_t btn_now = (uint16_t)((coin ? 1u : 0) | (start1 ? 2u : 0)
                           | (start2 ? 4u : 0) | (up ? 8u : 0)
                           | (down ? 16u : 0) | (left ? 32u : 0)
                           | (right ? 64u : 0) | (pepper ? 128u : 0)
                           | (rotate ? 256u : 0) | (mirror ? 512u : 0));
    static uint16_t btn_seen = 0;
    static uint16_t btn_combo = 0; // pepper seen TOGETHER with a direction
    btn_seen |= btn_now;
    if ((btn_now & 128u) && (btn_now & (8u | 16u | 32u | 64u)))
        btn_combo |= btn_now;

    // Frame-budget instrument -- the same one the other sketches carry.
    // `frame` alone means nothing, because hal_video_acquire_scanline()
    // BLOCKS until Core 1 frees a buffer: the loop measures
    // max(work, DVI frame period) and pins at ~16.7ms as soon as the work
    // fits. `work` is the real Core 0 cost. See DEVNOTES.md #16/#25.
    //
    // What to watch on this game specifically:
    //  - It runs TWO CPUs, both with their IRQ line checked between every
    //    instruction rather than once per scanline, because the main
    //    program's interrupt window is only ten cycles wide (DEVNOTES.md
    //    #52). That is a correctness requirement, not a tuning choice.
    //  - The synthesis runs six PSG channels at 187.5 kHz; `audio` below is
    //    its measured share of the frame.
    //  - The interpreters, the port decode, the render path and the
    //    synthesis are all placed in SRAM. On this board that is not a
    //    micro-optimisation: leaving the audio generator in flash alone
    //    cost 6.4ms of a 16.66ms frame (#60).
    static uint32_t frame_count = 0;
    // Totals beside every maximum -- see DEVNOTES #84/#85.
    static uint32_t work_max = 0, work_sum = 0, work_n = 0;
    static uint32_t blk_sum = 0, blk_max = 0;
    uint32_t t0 = micros();
    btime_run_frame(&g_system);
    uint32_t frame_us   = micros() - t0;
    uint32_t blocked_us = hal_video_take_blocked_us();
    uint32_t starve     = hal_video_take_starve_count();
    static uint32_t starve_total = 0;
    starve_total += starve;
    uint32_t work_us    = (frame_us > blocked_us) ? (frame_us - blocked_us) : 0;
    if (work_us > work_max) work_max = work_us;
    if (blocked_us > blk_max) blk_max = blocked_us;
    work_sum += work_us; blk_sum += blocked_us; work_n++;

    if ((++frame_count % 60u) == 0) {
        // The counters go out with the heartbeat because this machine's
        // two worst failure modes are both silent on screen: if the vblank
        // poll count is zero the program is not running at all, and if the
        // CPU-7 descramble count is zero the fetch hook is not working.
        // See tools/btime_host/main.cpp's header.
        btime_counters c;
        btime_debug_take_counters(&g_system, &c);

        Serial.print("[btime] frame ");
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
        Serial.print(starve_total);
        // Runway left at the worst moment -- see DEVNOTES #85.
        Serial.print(", minq ");
        Serial.print(hal_video_take_min_valid_level());
        Serial.print("/");
        Serial.print(hal_video_scanbuf_count());
        Serial.print("us | vblank ");
        Serial.print(c.vblank_reads);
        Serial.print(" swaps ");
        Serial.print(c.opcode_swaps);
        Serial.print(" snd ");
        Serial.print(c.latch_writes);
        Serial.print("/");
        Serial.print(c.ay_reg_writes);
        Serial.print(" ill ");
        Serial.print(c.illegal_ops);

        // Buttons: CSSudlrP.. as bits, then the pepper+direction witness.
        Serial.print(" | btn now ");
        Serial.print(btn_now, BIN);
        Serial.print(" seen ");
        Serial.print(btn_seen, BIN);
        Serial.print(" pepper+dir ");
        Serial.print(btn_combo, BIN);
        btn_seen = 0;
        btn_combo = 0;
        // Per-window, not lifetime -- a mean over the whole run since boot
        // would be dominated by attract and hide the gameplay peak.
        work_sum = blk_sum = work_n = 0; work_max = 0; blk_max = 0;

        // Sound health. `audio` is the measured cost of the synthesis
        // against the 16660us budget -- measured rather than inferred,
        // because an unmeasured audio cost is exactly what broke frame
        // pacing on Donkey Kong (DEVNOTES.md #48).
        uint32_t under = 0, over = 0, queued = 0;
        int32_t peak = 0;
        btime_audio_debug_take_stats(&under, &over, &queued, &peak);
        Serial.print(" | audio ");
        Serial.print(btime_audio_debug_cost_us());
        Serial.print("us q");
        Serial.print(queued);
        Serial.print(" under ");
        Serial.print(under);
        Serial.print(" peak ");
        Serial.print(peak);

        // Where the frame actually goes, measured on the device itself.
        // The host harness cannot answer this: it has no XIP, so it cannot
        // see flash-stall costs, and it got the audio share wrong by a
        // factor of three and a half. See DEVNOTES.md #60.
        uint32_t cpu_us = 0, render_us = 0, audio_cost = 0;
        btime_debug_take_costs(&cpu_us, &render_us, &audio_cost);
        Serial.print(" | cpu ");
        Serial.print(cpu_us);
        Serial.print(" render ");
        Serial.print(render_us);
        Serial.print(" snd ");
        Serial.print(audio_cost);
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
