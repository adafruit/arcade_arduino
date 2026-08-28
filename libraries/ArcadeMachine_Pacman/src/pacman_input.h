// Pac-Man input mapping.
//
// Deliberately does NOT depend on ArcadeHAL's hal_input contract or any
// board_config -- which physical button means "up" vs "coin" is a one-off
// decision for a specific Machine+Board pairing, decided in the sketch
// (see pacman_fruitjam.ino). This file only knows game-semantic booleans.
//
// IN0/IN1 bit layout verified against MAME's INPUT_PORTS_START(pacman)
// (src/mame/pacman/pacman.cpp) -- all bits active-low (0 = pressed/active).
#ifndef PACMAN_INPUT_H
#define PACMAN_INPUT_H

#include <stdbool.h>
#include "pacman_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

// Call once per frame with the CURRENT (already board-read) boolean level
// for each control -- true means "active/pressed". rotate_button and
// mirror_button are edge-detected internally (a press cycles rotation or
// toggles the mirror once, not once per frame the button is held). Only
// player 1's joystick is wired (this cabinet has no second joystick;
// `start2` still exists as its own coin-op button per MAME's IN1 bit 6).
void pacman_input_update(pacman_system *system,
                          bool coin, bool start1, bool start2,
                          bool up, bool down, bool left, bool right,
                          bool rotate_button, bool mirror_button);

#ifdef __cplusplus
}
#endif

#endif
