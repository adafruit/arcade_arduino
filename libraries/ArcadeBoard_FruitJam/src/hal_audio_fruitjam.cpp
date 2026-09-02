// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// hal_audio.h implementation for Adafruit Fruit Jam (TLV320DAC3100 + PIO I2S).
//
// Codec init and I2S PIO driver ported near-verbatim from invaders_pico's
// pico_sound.c (itself adapted from pico-infoNES/wili8jam), which comments
// this exact register sequence as "verified on Fruit Jam". The WAV mixer
// that used to live in this file is gone -- it's now board-agnostic game
// logic in ArcadeMachine_Invaders's invaders_audio.cpp, which registers
// itself here via hal_audio_set_fill_callback().
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "audio_i2s.pio.h"
#include "arcade_hal_audio.h"
#include "board_config_fruitjam.h"

// pio1, SM 0: DVI uses pio0 (see hal_video_fruitjam.cpp), so no conflict.
#define AUDIO_PIO      pio1
#define AUDIO_SM       0
// 256 samples, double-buffered. This was briefly lowered to 128 and then 64
// while chasing Lunar Rescue's red lines (DEVNOTES.md problem #34), on the
// reasoning that this ISR runs on Core 0, preempts the scanline render/submit
// pump, and PicoDVI's valid-scanline queue is a hard-capped 8 buffers -- only
// ~555us of slack -- so a long ISR can starve it. That reasoning was sound
// and the measurements were real: worst-single ISR cost fell 232us -> 81us ->
// 40-52us.
//
// It has been PUT BACK, and the reason is worth keeping. Shortening this was
// only ever an interim mitigation for Lunar Rescue, whose real fault was a
// ~1.8ms un-interleaved CPU burst leaving ~200us of margin. Interleaving that
// (problem #34) took the margin to milliseconds, at which point a 232us ISR
// is irrelevant -- but the mitigation's COST did not go away with its
// purpose. More, shorter calls pay the same fixed per-invocation overhead
// more often: measured on Galaga, the game with the least headroom, 64
// samples cost +400us mean / +660us peak per frame versus 256, and a red line
// appeared on hardware during heavy sprite activity with the player firing.
//
// **General lesson: when a real fix lands, remove the interim mitigation and
// re-measure. A workaround's cost outlives its purpose silently.** If a long
// ISR ever looks implicated again, measure `work` in that sketch's heartbeat
// first -- the frame budget is where this actually shows up.
#define BUFFER_SAMPLES 256

// ---------------------------------------------------------------------------
// Codec I2C helpers
// ---------------------------------------------------------------------------

static void codec_write_reg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    i2c_write_timeout_us(i2c0, FRUITJAM_DAC_I2C_ADDR, buf, 2, false, 1000);
}

static uint8_t codec_read_reg(uint8_t reg) {
    uint8_t v = reg;
    i2c_write_timeout_us(i2c0, FRUITJAM_DAC_I2C_ADDR, &v, 1, true, 1000);
    i2c_read_timeout_us(i2c0, FRUITJAM_DAC_I2C_ADDR, &v, 1, false, 1000);
    return v;
}

static void codec_modify_reg(uint8_t reg, uint8_t mask, uint8_t val) {
    codec_write_reg(reg, (codec_read_reg(reg) & ~mask) | (val & mask));
}

static void codec_set_page(uint8_t page) { codec_write_reg(0x00, page); }

// ---------------------------------------------------------------------------
// TLV320DAC3100 register init.
// GPIO 22 resets both DAC and the onboard ESP32-C6; we hold it high.
// DAC PLL derives its clock from BCLK -- no separate MCLK GPIO needed.
// ---------------------------------------------------------------------------

static void codec_init(void) {
    gpio_init(FRUITJAM_CODEC_RESET_PIN);
    gpio_set_dir(FRUITJAM_CODEC_RESET_PIN, GPIO_OUT);
    gpio_put(FRUITJAM_CODEC_RESET_PIN, true);

    i2c_init(i2c0, 100000);
    gpio_set_function(FRUITJAM_I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(FRUITJAM_I2C_SCL_PIN, GPIO_FUNC_I2C);
    sleep_ms(100);

    codec_write_reg(0x01, 0x01); // soft reset
    sleep_ms(10);

    // Audio interface: I2S 16-bit
    codec_modify_reg(0x1B, 0xC0, 0x00);
    codec_modify_reg(0x1B, 0x30, 0x00);

    // Clock MUX: PLL from BCLK
    codec_modify_reg(0x04, 0x03, 0x03);
    codec_modify_reg(0x04, 0x0C, 0x04);

    // PLL J=32, D=0
    codec_write_reg(0x06, 0x20);
    codec_write_reg(0x07, 0x00);
    codec_write_reg(0x08, 0x00);

    // PLL P/R
    codec_modify_reg(0x05, 0x0F, 0x02);
    codec_modify_reg(0x05, 0x70, 0x10);

    // NDAC=8, enable
    codec_modify_reg(0x0B, 0x7F, 0x08);
    codec_modify_reg(0x0B, 0x80, 0x80);

    // MDAC=2, enable
    codec_modify_reg(0x0C, 0x7F, 0x02);
    codec_modify_reg(0x0C, 0x80, 0x80);

    // NADC=8, enable; MADC=2, enable
    codec_modify_reg(0x12, 0x7F, 0x08);
    codec_modify_reg(0x12, 0x80, 0x80);
    codec_modify_reg(0x13, 0x7F, 0x02);
    codec_modify_reg(0x13, 0x80, 0x80);

    // PLL power up
    codec_modify_reg(0x05, 0x80, 0x80);

    // Headset detect
    codec_set_page(1);
    codec_modify_reg(0x2E, 0xFF, 0x0B);
    codec_set_page(0);
    codec_modify_reg(0x43, 0x80, 0x80);
    codec_modify_reg(0x30, 0x80, 0x80);
    codec_modify_reg(0x33, 0x3C, 0x14);

    // DAC power on (L+R)
    codec_modify_reg(0x3F, 0xC0, 0xC0);

    // DAC routing
    codec_set_page(1);
    codec_modify_reg(0x23, 0xC0, 0x40);
    codec_modify_reg(0x23, 0x0C, 0x04);

    // DAC volume: unmute, 0 dB
    codec_set_page(0);
    codec_modify_reg(0x40, 0x0C, 0x00);
    codec_write_reg(0x41, 0x00);
    codec_write_reg(0x42, 0x00);

    // ADC
    codec_modify_reg(0x51, 0x80, 0x80);
    codec_modify_reg(0x52, 0x80, 0x00);
    codec_write_reg(0x53, 0x68);

    // Headphone driver + gain
    codec_set_page(1);
    codec_modify_reg(0x1F, 0xC0, 0xC0);
    codec_modify_reg(0x28, 0x04, 0x04);
    codec_modify_reg(0x29, 0x04, 0x04);
    codec_write_reg(0x24, 0x0A);
    codec_write_reg(0x25, 0x0A);
    codec_modify_reg(0x28, 0x78, 0x40);
    codec_modify_reg(0x29, 0x78, 0x40);

    // Speaker amp
    codec_modify_reg(0x20, 0x80, 0x80);
    codec_modify_reg(0x2A, 0x04, 0x04);
    codec_modify_reg(0x2A, 0x18, 0x08);
    codec_write_reg(0x26, 0x0A);

    codec_set_page(0);
}

// ---------------------------------------------------------------------------
// DMA double buffer + ISR (in RAM -- avoids flash stall during XIP)
// ---------------------------------------------------------------------------

static int32_t audio_buf[2][BUFFER_SAMPLES];
static int dma_ch_a, dma_ch_b;
static volatile hal_audio_fill_cb g_fill_cb = NULL;

static void __not_in_flash_func(audio_dma_irq_handler)(void) {
    if (dma_irqn_get_channel_status(1, dma_ch_a)) {
        dma_irqn_acknowledge_channel(1, dma_ch_a);
        if (g_fill_cb) g_fill_cb(audio_buf[0], BUFFER_SAMPLES);
        else memset(audio_buf[0], 0, sizeof(audio_buf[0]));
        dma_channel_set_read_addr(dma_ch_a, audio_buf[0], false);
        dma_channel_set_trans_count(dma_ch_a, BUFFER_SAMPLES, false);
    }
    if (dma_irqn_get_channel_status(1, dma_ch_b)) {
        dma_irqn_acknowledge_channel(1, dma_ch_b);
        if (g_fill_cb) g_fill_cb(audio_buf[1], BUFFER_SAMPLES);
        else memset(audio_buf[1], 0, sizeof(audio_buf[1]));
        dma_channel_set_read_addr(dma_ch_b, audio_buf[1], false);
        dma_channel_set_trans_count(dma_ch_b, BUFFER_SAMPLES, false);
    }
}

// ---------------------------------------------------------------------------
// I2S PIO + DMA init
// ---------------------------------------------------------------------------

static void i2s_init(uint32_t sample_rate) {
    uint offset = pio_add_program(AUDIO_PIO, &audio_i2s_program);
    audio_i2s_program_init(AUDIO_PIO, AUDIO_SM, offset,
                            FRUITJAM_I2S_DIN_PIN, FRUITJAM_I2S_BCLK_PIN);

    // Clock divider: sys_clock / (sample_rate * 64)
    {
        uint32_t sys_hz = clock_get_hz(clk_sys);
        uint32_t target = sample_rate * 64u;
        uint32_t div_int  = sys_hz / target;
        uint32_t div_frac = (uint32_t)(((uint64_t)(sys_hz % target) * 256u) / target);
        pio_sm_set_clkdiv_int_frac(AUDIO_PIO, AUDIO_SM,
                                   (uint16_t)div_int, (uint8_t)div_frac);
    }

    dma_ch_a = dma_claim_unused_channel(true);
    dma_ch_b = dma_claim_unused_channel(true);
    memset(audio_buf, 0, sizeof(audio_buf));

    dma_channel_config cfg = dma_channel_get_default_config(dma_ch_a);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&cfg, true);
    channel_config_set_write_increment(&cfg, false);
    channel_config_set_dreq(&cfg, pio_get_dreq(AUDIO_PIO, AUDIO_SM, true));
    channel_config_set_chain_to(&cfg, dma_ch_b);
    dma_channel_configure(dma_ch_a, &cfg,
        &AUDIO_PIO->txf[AUDIO_SM], audio_buf[0], BUFFER_SAMPLES, false);

    cfg = dma_channel_get_default_config(dma_ch_b);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&cfg, true);
    channel_config_set_write_increment(&cfg, false);
    channel_config_set_dreq(&cfg, pio_get_dreq(AUDIO_PIO, AUDIO_SM, true));
    channel_config_set_chain_to(&cfg, dma_ch_a);
    dma_channel_configure(dma_ch_b, &cfg,
        &AUDIO_PIO->txf[AUDIO_SM], audio_buf[1], BUFFER_SAMPLES, false);

    dma_irqn_set_channel_enabled(1, dma_ch_a, true);
    dma_irqn_set_channel_enabled(1, dma_ch_b, true);
    irq_set_exclusive_handler(DMA_IRQ_1, audio_dma_irq_handler);
    irq_set_enabled(DMA_IRQ_1, true);

    pio_sm_set_enabled(AUDIO_PIO, AUDIO_SM, true);
    dma_channel_start(dma_ch_a);
}

// ---------------------------------------------------------------------------
// Public API (hal_audio.h)
// ---------------------------------------------------------------------------

bool hal_audio_init(uint32_t sample_rate) {
    codec_init();
    i2s_init(sample_rate);
    return true;
}

void hal_audio_set_fill_callback(hal_audio_fill_cb cb) {
    g_fill_cb = cb;
}

uint32_t hal_audio_enter_critical(void) {
    return save_and_disable_interrupts();
}

void hal_audio_exit_critical(uint32_t saved_state) {
    restore_interrupts(saved_state);
}
