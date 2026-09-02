// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Burger Time sound -- register plumbing only for now; see btime_audio.h
// for what is and is not implemented and why the sequencing is deliberate.
#include <string.h>
#include "btime_audio.h"
#include "arcade_hal_audio.h"

// Two AY-3-8910s. The sound CPU writes an address to one 8K window and data
// to another (audio_map(): 0x4000-0x5FFF and 0x2000-0x3FFF for ay1,
// 0x8000-0x9FFF and 0x6000-0x7FFF for ay2), so the latch is per chip.
static struct {
    uint8_t regs[16];
    uint8_t addr_latch;
} g_ay[2];

static uint32_t g_reg_writes;

void btime_audio_address_w(uint8_t chip, uint8_t value) {
    if (chip > 1) return;
    // The AY latches the low 4 bits as the register index; the upper bits
    // are the chip-select field on a real 8910 and are not modelled (both
    // chips here have their own address window, so selection is by address
    // rather than by those bits).
    g_ay[chip].addr_latch = (uint8_t)(value & 0x0F);
}

void btime_audio_data_w(uint8_t chip, uint8_t value) {
    if (chip > 1) return;
    g_ay[chip].regs[g_ay[chip].addr_latch] = value;
    g_reg_writes++;
}

// Silence, for now. Registered so the whole audio path -- HAL init, the
// fill callback, the board's I2S/DMA plumbing -- is exercised from the
// start rather than switched on for the first time along with the
// synthesis. `count` samples of stereo-interleaved silence.
static void fill_audio(int32_t *out, int count) {
    memset(out, 0, (size_t)count * 2u * sizeof(int32_t));
}

void btime_audio_init(void) {
    memset(g_ay, 0, sizeof(g_ay));
    g_reg_writes = 0;
    hal_audio_set_fill_callback(&fill_audio);
}

void btime_audio_run_slice(uint32_t slice, uint32_t slice_count) {
    (void)slice;
    (void)slice_count;
    // Nothing to generate yet. The call site exists now so that the
    // interleaving is in place before there is any cost to interleave --
    // adding it afterwards is what DEVNOTES.md #48 was.
}

uint32_t btime_audio_debug_cost_us(void) { return 0; }

uint32_t btime_audio_debug_take_reg_writes(void) {
    uint32_t n = g_reg_writes;
    g_reg_writes = 0;
    return n;
}
