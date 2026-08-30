// Donkey Kong sound -- silent stub. See dkong_audio.h for what the real
// hardware is and why it is not emulated yet.
#include <string.h>
#include "dkong_audio.h"
#include "arcade_hal_audio.h"
#include "pico.h" // __not_in_flash_func -- see ArcadeMachine_Invaders'
                  // invaders_audio.cpp for why this one include is a
                  // deliberate, documented exception to board-agnosticism
                  // (DEVNOTES.md problem #7)

// Runs in the board's audio ISR, so it is placed in SRAM for the same
// reason every other fill callback in this project is: an XIP cache-miss
// stall in the audio ISR is long enough to starve the PicoDVI scanline
// queue and show up as coloured glitch lines. Writing silence is cheap, but
// "cheap" is not "instant", and getting the placement right now means the
// attribute is already correct when this function grows a synthesiser.
// `count` is the number of int32_t ENTRIES to write, each one packing both
// channels as (sample << 16) | (uint16_t)sample -- see arcade_hal_audio.h.
// It is not a frame count, and treating it as one writes twice the buffer.
void __not_in_flash_func(dkong_audio_fill)(int32_t *out, int count) {
    memset(out, 0, (size_t)count * sizeof(int32_t));
}

void dkong_audio_init(dkong_system *system) {
    (void)system;
    hal_audio_set_fill_callback(dkong_audio_fill);
}
