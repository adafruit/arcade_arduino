// Space Invaders input mapping.
//
// Deliberately does NOT depend on ArcadeHAL's hal_input contract or on any
// board_config: which physical button/pin means "coin" vs "shoot" is a
// one-off decision for a specific Machine+Board pairing, so that wiring
// lives in the sketch (the SAMP composition root) -- see
// invaders_fruitjam.ino. This file only knows game-semantic booleans.
//
// Ported from invaders_pico's pico_input.c, minus the GPIO reads.
#ifndef INVADERS_INPUT_H
#define INVADERS_INPUT_H

#include <stdbool.h>
#include "invaders_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

// Call once per frame with the CURRENT (already board-read) boolean level
// for each control -- true means "active/pressed". rotate_button and
// mirror_button are edge-detected internally (a press cycles rotation or
// toggles the mirror once, not once per frame the button is held).
void invaders_input_update(arcade_system *system,
                            bool coin, bool start1, bool start2,
                            bool left, bool right, bool shoot,
                            bool rotate_button, bool mirror_button);

#ifdef __cplusplus
}
#endif

#endif
