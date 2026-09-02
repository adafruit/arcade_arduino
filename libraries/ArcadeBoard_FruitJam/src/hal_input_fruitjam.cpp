// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// hal_input.h implementation for Adafruit Fruit Jam.
// Ported from invaders_pico's pico_input.c (minus game-semantic mapping,
// which now lives in the sketch -- see invaders_fruitjam.ino).
//
// DEBOUNCING lives here rather than in any game, because arcade_hal_input.h
// explicitly makes it the board backend's choice ("already-debounced-or-not
// (board's choice)"). Until 2026-08-30 there was none at all anywhere in
// this project, which was only harmless by luck: sketches call
// hal_input_read() once per frame, and 60Hz sampling filters ordinary
// sub-millisecond contact bounce whether you ask it to or not. See the
// filter's own comment below for the shape chosen and why it is asymmetric.
#include <stdint.h>
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "arcade_hal_input.h"
#include "board_config_fruitjam.h"

// Indexed by the HAL_BTN_* enum from board_config_fruitjam.h, keeping the
// pin table and the enum from silently drifting apart.
static const uint8_t pins[] = {
    [HAL_BTN_ROTATE] = 4,
    [HAL_BTN_MIRROR] = 5,
    [HAL_BTN_COIN]   = 45,
    [HAL_BTN_START1] = 6,
    [HAL_BTN_START2] = 7,
    [HAL_BTN_LEFT]   = 8,
    [HAL_BTN_RIGHT]  = 9,
    [HAL_BTN_SHOOT]  = 10,
    [HAL_BTN_UP]     = 43,
    [HAL_BTN_DOWN]   = 44,
};

const uint8_t HAL_INPUT_BUTTON_COUNT = sizeof(pins) / sizeof(pins[0]);

// --- Debounce ------------------------------------------------------------
//
// The filter is ASYMMETRIC: a press is reported the instant it is seen, and
// only a RELEASE has to be held before it is believed. That asymmetry is
// deliberate -- adding latency to the press edge would be felt immediately
// in a shooter, whereas latency on the release edge only limits how fast a
// button can be re-triggered.
//
// The hold-off is deliberately SHORT (one to two frames). An earlier
// version of this file carried a 150ms hold-off on the action button, added
// to bridge apparent 68-142ms "dropouts" that input_bounce_test_fruitjam
// measured mid-press on the fire button. That was a workaround for a
// misdiagnosis and has been removed. The real cause of Galaga's double shot
// was in galaga_51xx.cpp -- fire was reported as a level when the hardware
// pulses it, so the game fired one bullet per frame the button was held --
// and it reproduced in the host harness with perfectly clean synthetic
// input, which no button could have caused. With that fixed, the most
// likely reading of those "dropouts" is that they were genuine double-taps
// by a player who had learned to fight the bug; note the directions, which
// are held rather than tapped, never showed them.
//
// Keeping the long hold-off would now actively hurt: Galaga is played by
// tapping fire quickly, and 150ms caps that at about 5 shots/second. If
// double shots ever reappear on hardware WITH the 51XX fix in place, that
// is the point to suspect the contacts again -- and to reach for the
// profiler before reaching for a constant.
#define DEFAULT_RELEASE_HOLD_US  25000u   // 25ms: ordinary contact bounce,
                                          // 1-2 frames, imperceptible on a
                                          // direction and harmless on a tap.

typedef struct {
    bool     stable;         // the level reported to callers
    bool     pending;        // raw currently disagrees with `stable`
    uint32_t pending_since;  // when the disagreement started (us)
} debounce_t;

static debounce_t filt[sizeof(pins) / sizeof(pins[0])];

void hal_input_init(void) {
    for (uint8_t i = 0; i < HAL_INPUT_BUTTON_COUNT; i++) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_IN);
        gpio_pull_up(pins[i]);
        filt[i].stable        = false;
        filt[i].pending       = false;
        filt[i].pending_since = 0;
    }
}

// Undebounced level, straight off the pin. NOT part of the ArcadeHAL
// contract -- it exists for input_bounce_test_fruitjam, which measures what
// the contacts actually do and would be measuring its own filter if it went
// through hal_input_read(). Games must not use this.
bool hal_input_read_raw(uint8_t index) {
    if (index >= HAL_INPUT_BUTTON_COUNT) return false;
    return !gpio_get(pins[index]); // active-low
}

bool hal_input_read(uint8_t index) {
    if (index >= HAL_INPUT_BUTTON_COUNT) return false;

    bool raw = !gpio_get(pins[index]); // active-low
    debounce_t *f = &filt[index];

    if (raw == f->stable) {
        f->pending = false;            // raw agrees again; cancel any flip
        return f->stable;
    }

    // timer_hw->timerawl, not time_us_32(): a raw MMIO read with no
    // function call, per this project's rule from DEVNOTES problem #17
    // (the SDK's timestamp helpers are flash-resident). This is not ISR
    // context so a flash call would not be fatal here, but it is called
    // once per button per frame and the register read is strictly cheaper.
    uint32_t now = timer_hw->timerawl;
    if (!f->pending) {
        f->pending       = true;
        f->pending_since = now;
    }

    // A press is believed immediately; a release must persist. Note this
    // runs at whatever rate the caller polls (once per frame for every game
    // here), so the effective resolution is one frame -- fine for hold-offs
    // measured in tens of milliseconds, and the reason this filter does not
    // try to do sub-millisecond work it could not observe anyway.
    uint32_t need = raw ? 0u : DEFAULT_RELEASE_HOLD_US;
    if ((uint32_t)(now - f->pending_since) >= need) {
        f->stable  = raw;
        f->pending = false;
    }
    return f->stable;
}
