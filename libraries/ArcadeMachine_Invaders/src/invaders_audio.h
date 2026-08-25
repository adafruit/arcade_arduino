// Space Invaders sample-based sound mixer -- board-agnostic.
// Ported from invaders_pico's pico_sound.c, minus the TLV320DAC3100/I2S
// hardware bring-up (that's ArcadeBoard_FruitJam's hal_audio_init()). This
// file only knows "10 numbered WAV files, mixed in software" -- it loads
// them via ArcadeHAL storage and registers itself as the ArcadeHAL audio
// fill callback.
#ifndef INVADERS_AUDIO_H
#define INVADERS_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define INVADERS_NUM_SAMPLES      10
#define INVADERS_AUDIO_SAMPLE_RATE 22050 // Hz, output rate fed to hal_audio_init()

// Loads /samples/0.wav.."9.wav" (MAME sample numbering, remapped to game
// slots internally -- see invaders_audio.cpp's sample_map) via ArcadeHAL
// storage, and registers the mixer with hal_audio_set_fill_callback(). Call
// after hal_storage_mount() and hal_audio_init(INVADERS_AUDIO_SAMPLE_RATE).
// A missing individual sample file is non-fatal (that sound just won't
// play); returns the count (0..INVADERS_NUM_SAMPLES) of samples that DID
// load, so the caller can treat "0 loaded" as an asset-load failure.
int invaders_audio_load_samples(void);

// Starts/stops game sound slot `sample_num` (0..9, game-semantic index --
// see invaders_ports.cpp for which slot means what). Safe to call from the
// i8080 port-write path; internally interrupt-safe against the audio ISR.
void invaders_audio_play(int sample_num);
void invaders_audio_stop(int sample_num);

#ifdef __cplusplus
}
#endif

#endif
