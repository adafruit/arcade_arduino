// Lunar Rescue input mapping.
//
// Deliberately does NOT depend on ArcadeHAL's hal_input contract or on any
// board_config -- see ArcadeMachine_Invaders' invaders_input.h for why that
// wiring belongs in the sketch instead. This file only knows game-semantic
// booleans.
//
// Lunar Rescue's cabinet is a 2-way (left/right) joystick plus a single
// fire/action button -- confirmed via MAME's midw8080/8080bw.cpp
// INPUT_PORTS_START(lrescue), which includes sicv_base and, like it, reuses
// Space Invaders' own INVADERS_CONTROL_PORT_P1 macro (mw8080bw.cpp) for its
// player controls -- i.e. the same three-line (left/right/shot) control
// input Space Invaders itself uses, not a 4-way/8-way joystick.
#ifndef LRESCUE_INPUT_H
#define LRESCUE_INPUT_H

#include <stdbool.h>
#include "lrescue_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

// Call once per frame with the CURRENT (already board-read) boolean level
// for each control -- true means "active/pressed". rotate_button and
// mirror_button are edge-detected internally (a press cycles rotation or
// toggles the mirror once, not once per frame the button is held).
void lrescue_input_update(arcade_system *system,
                           bool coin, bool start1, bool start2,
                           bool left, bool right, bool shot,
                           bool rotate_button, bool mirror_button);

#ifdef __cplusplus
}
#endif

#endif
