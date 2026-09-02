// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Standalone smoke test for ArcadeBoard_FruitJam's hal_audio implementation.
//
// Plays a continuous 440 Hz tone through the TLV320DAC3100 -- no WAV
// mixer, no SD card, no CPU emulator. Exercises the real production
// hal_audio_fruitjam.cpp (codec register sequence + PIO I2S/DMA wiring) in
// isolation, so a bad codec init or I2S timing bug shows up here first.
//
// Expected result: a steady 440 Hz tone from the headphone/line-out jack.
// The Serial Monitor (115200 baud) should print "still running..." once a
// second the whole time -- if audio is silent but that keeps printing,
// Core 0 is fine and the problem is in codec_init()/i2s_init() or the
// physical DAC wiring, not in this sketch's tone generator.
#include <math.h>
#include <arcade_hal_audio.h>
// arduino-cli discovers libraries to link by scanning #include directives,
// not library.properties `depends=` -- this include is what actually pulls
// ArcadeBoard_FruitJam's hal_audio_fruitjam.cpp (the real implementation of
// the functions below) into the build.
#include <board_config_fruitjam.h>

#define SAMPLE_RATE 22050
#define TONE_HZ     440.0f
#define PI_F        3.14159265358979323846f

static float phase = 0.0f;

static void fill_tone(int32_t *buf, int count) {
    float step = 2.0f * PI_F * TONE_HZ / (float)SAMPLE_RATE;
    for (int i = 0; i < count; i++) {
        // Simple sine approximation via the standard library is fine here --
        // this is a one-off smoke test, not the real-time mixer.
        int16_t s = (int16_t)(sinf(phase) * 8000.0f); // modest volume, avoid clipping
        phase += step;
        if (phase > 2.0f * PI_F) phase -= 2.0f * PI_F;
        buf[i] = ((int32_t)s << 16) | (uint16_t)s;
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("Initializing TLV320DAC3100 + I2S...");
    hal_audio_init(SAMPLE_RATE);
    hal_audio_set_fill_callback(fill_tone);
    Serial.println("Tone should be playing now (440 Hz).");
}

void loop() {
    delay(1000);
    Serial.println("still running...");
}
