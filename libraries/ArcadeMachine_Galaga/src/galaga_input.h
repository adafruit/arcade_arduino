// Galaga input mapping -- IN0/IN1 bit layout verified against MAME's
// INPUT_PORTS_START(galaga) (src/mame/namco/galaga.cpp, fetched and read
// directly this session):
//
//   IN0: bit1=joystick right, bit3=joystick left (2-way -- this cabinet
//        has no up/down), bit5/bit7=cocktail-mode right/left (unused,
//        upright-only convention this project already uses elsewhere)
//   IN1: bit0=button1/fire, bit1=cocktail button1 (unused), bit2=start1,
//        bit3=start2, bit4=coin1, bit5=coin2, bit6=service1, bit7=service
//
// All bits active-low. These feed galaga_system's in0/in1 shadow bytes,
// which galaga_51xx.cpp's HLE reads as 4-bit nibbles (see that file's
// header comment) rather than the game CPU reading them directly off a
// flat address the way Pac-Man's IN0/IN1 work -- the bit LAYOUT is still
// the same underlying fact this file establishes either way.
#ifndef GALAGA_INPUT_H
#define GALAGA_INPUT_H

#include <stdbool.h>
#include "galaga_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

void galaga_input_update(galaga_system *system,
                          bool coin, bool start1, bool start2,
                          bool left, bool right, bool fire,
                          bool rotate_button, bool mirror_button);

#ifdef __cplusplus
}
#endif

#endif
