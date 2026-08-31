// Galaga's Namco WSG sound: 3-voice wavetable synthesis, board-agnostic
// (registers as the ArcadeHAL audio fill callback -- never touches a
// specific board's DAC/I2S code directly).
//
// This is the SAME chip, the same register layout and the same clock as
// Pac-Man's: MAME wires Galaga's 0x6800-0x681F writes to
// `namco_wsg_device::pacman_sound_w` -- literally Pac-Man's own handler --
// and configures the device identically with
// `NAMCO_WSG(config, m_namco_sound, MASTER_CLOCK/6/32)` (18.432MHz/6/32 =
// 96000Hz). So ArcadeMachine_Pacman's pacman_audio.cpp is not merely
// similar prior art here, it is the same synthesis problem already solved
// and verified on real hardware; this file follows it deliberately closely
// rather than re-deriving it. See pacman_audio.cpp's header comment for the
// full register-map and phase-accumulator derivation, which applies here
// unchanged.
//
// Two real differences from Pac-Man, both verified against galaga()'s
// machine_config:
//  - NO sound-enable latch. Pac-Man gates its mixer on a mainlatch bit;
//    Galaga's misclatch wires only irq1/irq2/nmion/sub-reset (q_out_cb<0>
//    through <3>), with nothing routed to the sound device, so Galaga's WSG
//    is always live.
//  - Different waveform PROM: `prom-1.1d` (the "namco" ROM region's first
//    256 bytes). `prom-2.5c` occupies the rest of that region and is
//    labelled "timing - not used" in MAME's own ROM_START, so it is loaded
//    by nothing here.
//
// SCOPE: this covers the WSG only. Galaga's explosion/noise channel comes
// from the 54XX custom (see galaga_54xx.h), which is a separate and much
// less finished job -- that chip stays a command latch for now, so
// explosions are silent while shots, dives and the theme are not.
#ifndef GALAGA_AUDIO_H
#define GALAGA_AUDIO_H

#include <stdint.h>
#include "galaga_machine.h"

#ifdef __cplusplus
extern "C" {
// DEBUG: total microseconds spent inside the audio ISR since the last call,
// the number of invocations, and the single worst invocation -- then resets
// all three. The average (total/calls) and the worst single call answer
// different questions: a sustained per-channel cost increase moves the
// average, while a rare outlier (a channel hitting a loop-restart edge
// case) moves only the max. DEVNOTES.md problem #35.
void galaga_audio_debug_take_isr_stats(uint32_t *total_us, uint32_t *calls,
                                       uint32_t *max_call_us);

#endif

#define GALAGA_AUDIO_SAMPLE_RATE 22050  // Hz, output rate fed to hal_audio_init()
#define GALAGA_WAVE_PROM_SIZE    0x0100 // prom-1.1d -- 8 waveforms x 32 samples

// Raw waveform PROM staging buffer -- filled by galaga_assets.cpp's loader.
// Each byte's low nibble is one 4-bit sample biased by 8 (value - 8 =
// -8..7); byte index = waveform_select*32 + phase_position(0-31).
extern uint8_t galaga_wave_prom[GALAGA_WAVE_PROM_SIZE];

// Registers the WSG mixer as the ArcadeHAL audio fill callback. Call after
// hal_audio_init(GALAGA_AUDIO_SAMPLE_RATE) and after galaga_wave_prom has
// been loaded. `system` is read (under hal_audio_enter_critical()) each
// time the fill callback runs, for the current wsg_regs state -- the chip
// is a free-running oscillator, so there is no play/stop API.
void galaga_audio_init(galaga_system *system);

#ifdef __cplusplus
}
#endif

#endif
