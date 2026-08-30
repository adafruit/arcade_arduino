// Ms. Pac-Man's Namco WSG sound -- IDENTICAL hardware to Pac-Man's, and
// this file is a verbatim copy of ArcadeMachine_Pacman's pacman_audio.h.
// The aux daughterboard adds no sound hardware; the same 82s126.1m
// waveform PROM is used, with the same CRC.
// ORIGINAL COMMENT FOLLOWS, unchanged:
//
// Pac-Man's Namco WSG sound: 3-voice wavetable synthesis, board-agnostic
// (registers as the ArcadeHAL audio fill callback -- never touches a
// specific board's DAC/I2S code directly).
//
// A third distinct sound architecture in this project, alongside
// Invaders' sample playback and Lunar Rescue's bit-banged single-bit
// channel: WSG is a continuously-running oscillator whose current pitch/
// waveform/volume are simply "whatever the registers currently say" --
// unlike Lunar Rescue's channel (which had to reconstruct discrete
// transition *events* in cycle-timestamped order, see
// arcade_arduino/DEVNOTES.md problem #15), this needs no event ring
// buffer: the audio fill callback just reads the current register state
// each time it runs, exactly like the real chip's own free-running
// per-voice phase accumulator does.
#ifndef MSPACMAN_AUDIO_H
#define MSPACMAN_AUDIO_H

#include <stdint.h>
#include "mspacman_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MSPACMAN_AUDIO_SAMPLE_RATE 22050 // Hz, output rate fed to hal_audio_init()
#define MSPACMAN_WAVE_PROM_SIZE    0x0100 // 82s126.1m -- 8 waveforms x 32 samples

// Raw waveform PROM staging buffer -- filled by mspacman_assets.cpp's
// loader. Each byte's low nibble is one signed-ish 4-bit sample (value - 8
// = -8..7); byte index = waveform_select*32 + phase_position(0-31).
// Verified against MAME's namco_wsg_device (Voices=3, Packed=false) and
// its non-packed waveform_r()/pacman_sound_w() register map -- see
// mspacman_audio.cpp's header comment for the exact register layout and
// frequency-accumulator formula this was derived from.
extern uint8_t mspacman_wave_prom[MSPACMAN_WAVE_PROM_SIZE];

// Registers the WSG mixer as the ArcadeHAL audio fill callback. Call after
// hal_audio_init(MSPACMAN_AUDIO_SAMPLE_RATE) and after mspacman_wave_prom has
// been loaded. `system` is read (under hal_audio_enter_critical()) each
// time the fill callback runs, for the current sound_regs/sound_enable
// state -- no separate "play"/"stop" API is needed, unlike the
// sample-based and bit-banged machines' audio contracts.
void mspacman_audio_init(mspacman_system *system);

#ifdef __cplusplus
}
#endif

#endif
