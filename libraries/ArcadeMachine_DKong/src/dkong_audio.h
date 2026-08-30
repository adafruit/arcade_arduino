// Donkey Kong sound -- NOT YET IMPLEMENTED. This is a silent stub.
//
// Real Donkey Kong has no sound chip. It has an MB8884 (an 8035-class
// MCS-48 microcontroller) running its own 2KB program, which drives a DAC
// directly and plays "voice" samples out of a second ROM in banked 256-byte
// pages -- that is where the music and the walking sound come from -- PLUS a
// discrete analog network (an LFSR noise source, RC filters and a custom
// mixer) for jump, boom and spring. See MAME's dkong_a.cpp:
// dkong2b_audio() and DISCRETE_SOUND_START(dkong2b_discrete).
//
// Emulating it means adding a third CPU axis to this framework
// (ArcadeCPU_MCS48) plus an approximation of the discrete network, which is
// a port of its own. The main CPU does not depend on any of it to run: every
// sound path in dkong_map() is write-only, and the one readable line -- the
// sound CPU's status, IN2 bit 6 -- is handled in dkong_input.cpp.
//
// This stub registers a fill callback that writes silence, so the board's
// audio hardware is initialised and running with a well-defined source.
// That is deliberate rather than skipping hal_audio_init() entirely: it
// keeps the DAC/I2S path exercised on every boot, so whoever adds the 8035
// finds a working audio pipeline and only has to fill the buffer.
#ifndef DKONG_AUDIO_H
#define DKONG_AUDIO_H

#include "dkong_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

// 22050 Hz, matching every other machine in this project. There is no
// game-specific reason for this value here -- nothing is synthesised yet --
// and deliberately not deviating keeps one fewer variable in play when the
// board's audio path is being blamed for something.
#define DKONG_AUDIO_SAMPLE_RATE 22050

// Registers the (silent) ArcadeHAL audio fill callback.
void dkong_audio_init(dkong_system *system);

#ifdef __cplusplus
}
#endif

#endif
