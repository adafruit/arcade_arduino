// Donkey Kong input mapping.
//
// Deliberately does NOT depend on ArcadeHAL's hal_input contract or any
// board_config -- which physical button means "jump" vs "coin" is a one-off
// decision for a specific Machine+Board pairing, decided in the sketch (see
// dkong_fruitjam.ino). This file only knows game-semantic booleans.
#ifndef DKONG_INPUT_H
#define DKONG_INPUT_H

#include <stdbool.h>
#include "dkong_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

// Call once per frame with the CURRENT (already board-read) boolean level
// for each control -- true means "active/pressed". rotate_button and
// mirror_button are edge-detected internally.
void dkong_input_update(dkong_system *system,
                        bool coin, bool start1, bool start2,
                        bool up, bool down, bool left, bool right,
                        bool jump,
                        bool rotate_button, bool mirror_button);

#ifdef __cplusplus
}
#endif

#endif
