// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Burger Time sound: a second 6502 driving two AY-3-8910 PSGs into a small
// discrete network.
//
// Verified against btime()'s machine_config, audio_map(),
// audio_nmi_enable_w()/audio_nmi_gen() and DISCRETE_SOUND_START(
// btime_sound_discrete) in src/mame/dataeast/btime.cpp, plus
// src/devices/sound/ay8910.cpp for the chip itself.
//
// WHAT THE REAL BOARD DOES
//
//   - A dedicated M6502 at 500 kHz runs ab14.12h. The main CPU hands it
//     one-byte commands through a latch and an IRQ; a scanline timer gives
//     it a ~976 Hz NMI tick (gated by a software enable) to sequence music.
//   - TWO AY-3-8910s at 1.5 MHz produce all six channels. Nothing else on
//     this board makes sound -- no DAC, no samples.
//   - The analog side is unusually small for this project: channels 1A, 1B,
//     1C, 2B and 2C are summed flat and scaled by 0.2, channel 2A ALONE
//     goes through a band-pass op-amp filter, the two are mixed, and two
//     high-passes follow. MAME does not model the uPC1181H amplifier.
//
// WHAT THIS PORT DOES
//
// The chip is EMULATED, following MAME's ay8910_device: tone, noise and
// envelope generators clocked at clock/8, and MAME's own resistor-ladder
// amplitude tables evaluated offline for this board's two load resistances.
//
// The network is IMPLEMENTED, not approximated, with one documented
// exception. Channel 2A's band-pass is realised as the same second-order
// section MAME derives from the measured component values (a ~187 Hz peak,
// Q ~1.9, 7x gain -- it is the BASS channel); the flat 0.2 sum, the op-amp
// mixer's 0.1 per input and the 10k/10uF DC blocker are all direct. What is
// deliberately left out is the SECOND high-pass, which models the cabinet's
// 4-ohm speaker at 530 Hz: the Fruit Jam has its own speaker with its own
// response, and stacking an arcade cabinet's speaker model on top of a
// different real one models the wrong thing twice. See btime_audio.cpp.
//
// The one value that is NOT derived is the final output level, for the
// reason it never is in this project: MAME's netlist gain is in volt-ish
// units that do not survive the change of amplitude representation, so it
// was set by measuring the actual peak in the host harness and leaving
// headroom. Only real hardware can judge it.
#ifndef BTIME_AUDIO_H
#define BTIME_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 22050 Hz, matching every other machine in this project.
#define BTIME_AUDIO_SAMPLE_RATE 22050

// AY-3-8910 clock: 12_MHz_XTAL / 2 / 2 / 2 = 1.5 MHz, from
// AY8910(config, "ay1", 12_MHz_XTAL / 2 / 2 / 2) in btime(). MAME runs its
// AY model at clock/8 (stream_alloc(..., master_clock / 8)), which is the
// rate the tone/noise/envelope counters advance at.
#define BTIME_AY_CLOCK      1500000u
#define BTIME_AY_STEP_RATE  (BTIME_AY_CLOCK / 8u) // 187500 Hz

// Registers up an address latch (chip 0 = ay1, chip 1 = ay2). Called from
// btime_ports.cpp's sound-CPU write decode; the address and data ports are
// separate 8K windows in the sound CPU's map.
void btime_audio_address_w(uint8_t chip, uint8_t value);
void btime_audio_data_w(uint8_t chip, uint8_t value);

// Registers the HAL fill callback and clears state. Call once after
// hal_audio_init().
void btime_audio_init(void);

// Produces this frame's audio in slices, called from inside the scanline
// loop rather than once at the end of the frame. DEVNOTES.md problem #48 is
// the record of why: Donkey Kong's frame had its CPU carefully interleaved
// and then a 2.9ms un-interleaved audio burst bolted onto the end, and
// 9.4 + 2.9ms of a 16.66ms budget still broke frame pacing outright. The
// rule is not "is there budget" but "is there ever a gap longer than ~2ms
// between two scanline submissions".
void btime_audio_run_slice(uint32_t slice, uint32_t slice_count);

// Mean microseconds per frame spent generating audio over the last 60
// frames -- the sound half's real cost against the 16660us budget,
// measured rather than inferred (DEVNOTES.md #48).
uint32_t btime_audio_debug_cost_us(void);

// Ring-buffer health and the peak sample seen since the last call, then
// resets all four. `underruns` counts times the ISR found the ring empty --
// nonzero means Core 0 is not keeping up and the sound will stutter, which
// is a frame-budget symptom rather than a synthesis one. `peak` is what the
// OUTPUT_GAIN constant in btime_audio.cpp was set against.
void btime_audio_debug_take_stats(uint32_t *out_underruns, uint32_t *out_overruns,
                                  uint32_t *out_queued, int32_t *out_peak);

// Total AY register writes since the last call, then resets. Answers "is
// the sound CPU actually talking to the PSGs" with a number while the
// synthesis is still missing -- if this is zero, no amount of waveform code
// would make a sound.
uint32_t btime_audio_debug_take_reg_writes(void);

#ifdef __cplusplus
}
#endif

#endif
