// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Standalone smoke test for ArcadeBoard_FruitJam's hal_video implementation.
//
// Draws scrolling color bars -- no CPU emulator, no ROM, no SD card. This
// exercises the real production hal_video_fruitjam.cpp (PicoDVI scanline
// queue, adafruit_fruitjam_cfg pinout, the Core 0/Core 1 split) in
// isolation, so a bad HDMI cable, a wiring problem, or a DVI timing bug
// shows up here first instead of being tangled up with CPU/audio/SD issues
// in the full game build.
//
// Expected result: an HDMI monitor connected to the Fruit Jam shows 8
// scrolling vertical color bars (white/yellow/cyan/green/magenta/red/blue/
// black). The Serial Monitor (115200 baud) should print a heartbeat line
// roughly once per second -- if the bars are frozen but the heartbeat is
// still printing, Core 0 is fine and the problem is downstream in Core 1's
// hal_video_run() pump; if nothing prints at all, Core 0 is stuck.
#include <arcade_hal_video.h>
// arduino-cli discovers libraries to link by scanning #include directives,
// not library.properties `depends=` -- this include is what actually pulls
// ArcadeBoard_FruitJam's hal_video_fruitjam.cpp (the real implementation of
// the functions below) into the build.
#include <board_config_fruitjam.h>

static volatile bool g_video_ready = false;

void setup() {
    // PicoDVI's 640x480 mode requires the system clock to equal the TMDS
    // bit clock (252 MHz) -- must happen before any other peripheral init.
    set_sys_clock_khz(252000, true);

    Serial.begin(115200);
    hal_video_init();
    g_video_ready = true;
}

void setup1() {
    while (!g_video_ready) {
        tight_loop_contents();
    }
    hal_video_run(); // never returns
}

void loop1() {
    // Unreachable -- hal_video_run() in setup1() never returns.
}

static uint16_t bar_color(uint32_t band) {
    switch (band % 8) {
    case 0: return 0xFFFF; // white
    case 1: return 0xFFE0; // yellow
    case 2: return 0x07FF; // cyan
    case 3: return 0x07E0; // green
    case 4: return 0xF81F; // magenta
    case 5: return 0xF800; // red
    case 6: return 0x001F; // blue
    default: return 0x0000; // black
    }
}

void loop() {
    static uint32_t frame = 0;
    uint32_t band_width = HAL_VIDEO_WIDTH / 8;

    // Submit exactly HAL_VIDEO_HEIGHT times per frame, filling
    // HAL_VIDEO_WIDTH pixels each. Those are the CANVAS dimensions
    // (320x240 here), not the physical 640x480: the board doubles both
    // axes on the way out, and that is its business, not ours. See
    // arcade_hal_video.h -- this used to require submitting
    // HAL_VIDEO_SCANLINES_PER_FRAME times against a HAL_VIDEO_HEIGHT of
    // 480, and getting that pairing wrong is what DEVNOTES #76 is about.
    for (uint32_t i = 0; i < HAL_VIDEO_HEIGHT; i++) {
        uint16_t *buf = hal_video_acquire_scanline();
        for (uint32_t x = 0; x < HAL_VIDEO_WIDTH; x++) {
            buf[x] = bar_color((x + frame) / band_width);
        }
        hal_video_submit_scanline(buf);
    }

    frame++;
    if (frame % 60 == 0) {
        Serial.print("frame ");
        Serial.println(frame);
    }
}
