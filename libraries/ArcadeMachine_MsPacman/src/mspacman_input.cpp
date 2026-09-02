// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Ms. Pac-Man input mapping -- IN0/IN1 bit layout verified against MAME's
// INPUT_PORTS_START(mspacman) (src/mame/pacman/pacman.cpp). That is its
// own port definition rather than a PORT_INCLUDE of pacman's, but the
// IN0/IN1 halves are bit-for-bit the same; only DSW1 differs, and that is
// handled in mspacman_assets.cpp. Layout:
//
//   IN0 (0x5000): bit0=up bit1=left bit2=right bit3=down bit4=rack test
//                 bit5=coin1 bit6=coin2 bit7=service1
//   IN1 (0x5040): bit0=up(P2) bit1=left(P2) bit2=right(P2) bit3=down(P2)
//                 bit4=service bit5=start1 bit6=start2 bit7=cabinet
//
// All bits active-low. This cabinet has one joystick (wired to IN0's
// up/left/right/down, matching an upright cabinet -- IN1's P2 joystick
// bits are left permanently inactive, same as leaving DSW's "Cabinet" bit
// at its "Upright" default). Rack test/coin2/service1/service/cabinet are
// not wired to any physical control -- left at their inactive (1) level.
#include "mspacman_input.h"

void mspacman_input_update(mspacman_system *system,
                          bool coin, bool start1, bool start2,
                          bool up, bool down, bool left, bool right,
                          bool rotate_button, bool mirror_button) {
    system->in0 = (uint8_t)(0xFF
        & ~(up    ? 0x01 : 0)
        & ~(left  ? 0x02 : 0)
        & ~(right ? 0x04 : 0)
        & ~(down  ? 0x08 : 0)
        & ~(coin  ? 0x20 : 0));

    system->in1 = (uint8_t)(0xFF
        & ~(start1 ? 0x20 : 0)
        & ~(start2 ? 0x40 : 0));

    // Edge-detected meta controls -- a press cycles rotation or toggles
    // the mirror once, not once per frame the button is held. Same shape
    // as invaders_input.cpp's rotate/mirror handling.
    static bool prev_rotate = false, prev_mirror = false;
    if (rotate_button && !prev_rotate) system->rotation = (system->rotation + 1) & 0x03;
    if (mirror_button && !prev_mirror) system->mirror_x = !system->mirror_x;
    prev_rotate = rotate_button;
    prev_mirror = mirror_button;
}
