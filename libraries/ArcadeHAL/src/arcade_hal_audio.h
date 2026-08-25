// ArcadeHAL: audio contract.
//
// A board backend implements this by turning a stream of PCM sample
// buffers into sound on its physical DAC/speaker path. It does NOT know
// about WAV files, sample indices, or mixing -- that's Machine-layer logic
// (see ArcadeMachine_Invaders's invaders_audio.*), which registers itself
// as the fill callback. This contract only knows how to move filled sample
// buffers to a codec.
#ifndef ARCADE_HAL_AUDIO_H
#define ARCADE_HAL_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Fills one block of `count` interleaved 16-bit stereo frames into `out`,
// packed as the reference mixer does: each entry is
// (sample << 16) | (uint16_t)sample -- i.e. left and right channels
// carrying the same mono mix. Called from the board's audio ISR/DMA
// completion handler, so implementations must be fast and must not touch
// flash/XIP (see invaders_pico's DEVNOTES.md "Red horizontal lines when
// sounds play" for why -- a stalled fill callback here can glitch video on
// boards that share timing budget between audio and video peripherals).
typedef void (*hal_audio_fill_cb)(int32_t *out, int count);

// One-time setup: init the codec and whatever I2S/DMA plumbing moves audio
// out, at the given output sample rate. Playback produces silence until a
// fill callback is registered with hal_audio_set_fill_callback().
bool hal_audio_init(uint32_t sample_rate);

// Registers the function the board's audio ISR calls to fill each output
// buffer. Exactly one callback is active at a time.
void hal_audio_set_fill_callback(hal_audio_fill_cb cb);

// Critical section around state shared with the audio ISR (e.g. a Machine's
// mixer channel array). What this means is entirely board-specific --
// disabling interrupts on a bare-metal RP2350 build, a mutex under an RTOS,
// etc. -- so Machine code must go through this rather than reaching for a
// platform interrupt-control header directly. Non-reentrant: do not nest.
uint32_t hal_audio_enter_critical(void);
void hal_audio_exit_critical(uint32_t saved_state);

#ifdef __cplusplus
}
#endif

#endif
