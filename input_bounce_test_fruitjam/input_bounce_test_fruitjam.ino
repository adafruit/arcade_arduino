// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Contact-bounce profiler for ArcadeBoard_FruitJam's buttons.
//
// WHY THIS EXISTS, and why input_test_fruitjam.ino can't answer the same
// question: that sketch polls at 50Hz behind a `delay(20)`, which is
// itself a crude debounce -- it proves a button is wired to the right pin,
// and is deliberately blind to what the contact does electrically. This
// sketch does the opposite: it samples at 10kHz (a repeating hardware
// timer, not the loop) and reports EVERY transition, so a switch that
// chatters is visible instead of being silently filtered.
//
// The question it was written to answer: Galaga fires two shots from one
// press of the fire button on real hardware, while the host harness -- fed
// an ideal, bounce-free press of any length from 1 to 110 frames -- fires
// exactly one. Galaga's ROM edge-detects the fire button, so a second shot
// means the game genuinely saw a second press. At the time, the input path
// had no debouncing at all (sketches sample a raw `gpio_get()` once per
// frame), so a contact that opened for longer than a frame would manufacture
// exactly that -- which is what this sketch went looking for, and found.
//
// So the report below prints two things per press:
//   1. the real transitions, with microsecond timing;
//   2. what a once-per-frame (60Hz) sampler would have seen -- which is
//      what the game actually gets -- and how many PRESS EDGES that
//      produces. Any press whose frame-sampled view contains more than one
//      rising edge is a double-fire waiting to happen.
//
// It samples hal_input_read_RAW() deliberately -- straight off the pin,
// bypassing the debounce filter hal_input_read() now applies (see
// hal_input_fruitjam.cpp). Going through the filter would mean measuring
// the filter instead of the contacts, which is exactly backwards for a tool
// whose job is to decide how the filter should be tuned. The report shows
// both: the raw transitions, and what the filter turns them into.
//
// Expected result on a healthy button: a handful of transitions inside a
// couple of milliseconds, and "frame-sampled press edges: 1".
//
// WHAT IT FOUND (2026-08-30), and the important caveat: it measured real
// 68-142ms openings mid-press on the fire button. That looked like the
// cause of Galaga's double shot. **It was not.** The double shot was a
// level-vs-pulse bug in galaga_51xx.cpp (fire reported as a level, so the
// game fired a bullet per frame the button was held) which reproduced in
// the host harness with perfectly clean synthetic input -- no button could
// have caused it. See DEVNOTES problem #32.
//
// So treat this sketch as evidence about the CONTACTS and nothing else, and
// clear the emulation first: the harness is cheaper and, in that instance,
// was the only thing that could have given the right answer. Two specific
// traps that cost a session:
//   - a swap test (move fire and left to each other's pins) does NOT
//     isolate a button, because the button and the WAY A PERSON PRESSES IT
//     move together;
//   - a player who has adapted to a firing bug may double-tap without
//     realising, which looks exactly like a dropout. Note that joystick
//     directions -- held rather than tapped -- never showed any.
#include <Arduino.h>
#include <arcade_hal_input.h>
#include <board_config_fruitjam.h>
#include "pico/time.h"
#include <stdarg.h>
#include <stdio.h>

static const char *names[] = {
    "ROTATE (B2)", "MIRROR (B3)", "COIN", "START1", "START2",
    "LEFT", "RIGHT", "SHOOT", "UP", "DOWN"
};

#define SAMPLE_US    100    // 10kHz -- fast enough to resolve contact bounce
#define MAX_EVENTS   256
#define SETTLE_US    400000 // report a burst once nothing has moved for 400ms
#define FRAME_US     16667  // what the game's once-per-frame sampler sees

typedef struct { uint32_t us; uint8_t btn; uint8_t level; } event_t;

static volatile event_t   events[MAX_EVENTS];
static volatile uint16_t  event_n    = 0;
static volatile uint32_t  last_us    = 0;
static bool               last_level[16];
static repeating_timer_t  timer;

static bool sample_cb(repeating_timer_t *t) {
    (void)t;
    uint32_t now = micros();
    for (uint8_t i = 0; i < HAL_INPUT_BUTTON_COUNT; i++) {
        bool now_level = hal_input_read_raw(i); // RAW -- see below
        if (now_level != last_level[i]) {
            last_level[i] = now_level;
            if (event_n < MAX_EVENTS) {
                events[event_n].us    = now;
                events[event_n].btn   = i;
                events[event_n].level = now_level ? 1 : 0;
                event_n++;
            }
            last_us = now;
        }
    }
    return true;
}

static event_t  snap[MAX_EVENTS];
static uint16_t snap_n;

// Reports are assembled in one buffer and pushed with a single write +
// flush, rather than by a few dozen Serial.print() calls. The first version
// did the latter and visibly LOST CHARACTERS mid-report -- USB CDC drops
// rather than blocks when its TX buffer fills, and a long report issued
// from a sketch that is also taking a 10kHz timer interrupt fills it
// easily. If output still looks truncated, lower MAX_PRINTED_EVENTS rather
// than adding more print calls.
#define REPORT_BUF     4096
#define MAX_PRINTED_EVENTS 48
static char     rbuf[REPORT_BUF];
static uint16_t rlen;

static void rprintf(const char *fmt, ...) {
    if (rlen >= REPORT_BUF - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(rbuf + rlen, REPORT_BUF - rlen, fmt, ap);
    va_end(ap);
    if (n > 0) rlen = (uint16_t)((rlen + n < REPORT_BUF) ? rlen + n : REPORT_BUF - 1);
}

// Replays one button's transition list through a 60Hz sampler -- the same
// once-per-frame read the game does -- and counts rising edges. This is
// the number that matters: it is literally how many presses the game sees.
static void report_frame_view(uint8_t btn, uint32_t t0, uint32_t t1) {
    char view[64];
    uint8_t vn = 0;
    int edges = 0;
    bool prev = false;
    for (uint32_t t = t0; t <= t1 + FRAME_US; t += FRAME_US) {
        // Level at time t = the level set by the last transition at or
        // before t (starting from "released" before the first one).
        bool level = false;
        for (uint16_t i = 0; i < snap_n; i++) {
            if (snap[i].btn != btn) continue;
            if ((int32_t)(snap[i].us - t) > 0) break;
            level = snap[i].level != 0;
        }
        if (vn < sizeof(view) - 1) view[vn++] = level ? '1' : '0';
        if (level && !prev) edges++;
        prev = level;
    }
    view[vn] = 0;
    rprintf("    frame-sampled (60Hz) view: %s  -> press edges: %d\n", view, edges);
    if (edges > 1) {
        rprintf("    *** MORE THAN ONE PRESS EDGE -- this single physical press\n"
                "    *** would fire twice in Galaga. Look for a gap of tens of ms\n"
                "    *** in the timing above: that is the contact dropping out,\n"
                "    *** not bounce (bounce is the sub-millisecond stuff).\n");
    }
}

// Snapshot of the ring, taken once so printing can't race the sampler.
// Printing a report takes tens of milliseconds at 115200 baud, which is
// long enough for a new press to arrive mid-report -- the first version of
// this sketch printed straight from the live buffer and produced visibly
// interleaved, garbled lines.
static void report(void) {
    noInterrupts();
    snap_n = event_n;
    for (uint16_t i = 0; i < snap_n; i++) {
        // Field-by-field: a volatile struct can't be copied wholesale in C++.
        snap[i].us    = events[i].us;
        snap[i].btn   = events[i].btn;
        snap[i].level = events[i].level;
    }
    event_n = 0;
    interrupts();

    uint16_t n = snap_n;
    if (n == 0) return;

    rlen = 0;
    rprintf("\n--- burst: %u transition(s) ---\n", (unsigned)n);

    // Per button that moved at all.
    for (uint8_t b = 0; b < HAL_INPUT_BUTTON_COUNT; b++) {
        uint16_t count = 0;
        uint32_t first = 0, last = 0;
        for (uint16_t i = 0; i < n; i++) {
            if (snap[i].btn != b) continue;
            if (count == 0) first = snap[i].us;
            last = snap[i].us;
            count++;
        }
        if (count == 0) continue;

        rprintf("  %s: %u transitions spanning %lu.%02lu ms\n",
                names[b], (unsigned)count,
                (unsigned long)((last - first) / 1000u),
                (unsigned long)(((last - first) % 1000u) / 10u));

        rprintf("    timing (ms from first, level): ");
        uint16_t printed = 0;
        for (uint16_t i = 0; i < n; i++) {
            if (snap[i].btn != b) continue;
            if (printed++ >= MAX_PRINTED_EVENTS) { rprintf("... "); break; }
            uint32_t d = snap[i].us - first;
            rprintf("%lu.%02lu%c ", (unsigned long)(d / 1000u),
                    (unsigned long)((d % 1000u) / 10u),
                    snap[i].level ? '^' : 'v');
        }
        rprintf("\n");

        report_frame_view(b, first, last);
    }

    Serial.write((const uint8_t *)rbuf, rlen);
    Serial.flush();
}

void setup() {
    Serial.begin(115200);
    delay(1500);
    hal_input_init();
    for (uint8_t i = 0; i < HAL_INPUT_BUTTON_COUNT; i++)
        last_level[i] = hal_input_read_raw(i);

    Serial.println();
    Serial.println("=== Fruit Jam contact-bounce profiler ===");
    Serial.print("Sampling every ");
    Serial.print(SAMPLE_US);
    Serial.println(" us (10 kHz) via a hardware repeating timer.");
    Serial.println("Press a button normally, as you would while playing.");
    Serial.println("A report prints 400 ms after the button settles.");
    Serial.println("Watch the 'press edges' line: it should be 1 per press.");
    Serial.println();

    add_repeating_timer_us(-SAMPLE_US, sample_cb, NULL, &timer);
}

void loop() {
    noInterrupts();
    uint16_t n  = event_n;
    uint32_t lu = last_us;
    interrupts();

    if (n > 0 && (uint32_t)(micros() - lu) > SETTLE_US) report();
    delay(10);
}
