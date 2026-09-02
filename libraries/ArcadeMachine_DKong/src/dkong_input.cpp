// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Donkey Kong input mapping -- IN0/IN1/IN2 bit layout verified against
// MAME's INPUT_PORTS_START( dkong_in0_4 / dkong_in1_4 / dkong_in2 )
// (src/mame/nintendo/dkong.cpp):
//
//   IN0 (0x7C00): bit0=right bit1=left bit2=up bit3=down bit4=jump(BUTTON1)
//                 bits5-7 not connected
//   IN1 (0x7C80): same layout, cocktail player 2
//   IN2 (0x7D00): bit0=service bit2=start1 bit3=start2
//                 bit6=sound CPU status  bit7=coin1
//
// EVERY BIT HERE IS ACTIVE HIGH. That is the opposite of every other game
// in this project, and it is why this file builds its bytes by OR-ing bits
// in rather than masking them out of 0xFF. It also means the memset-zero
// state of a fresh dkong_system is already the correct "nothing pressed"
// idle -- the exact inverse of the trap recorded in
// tools/host_common/hal_host.cpp, where Galaga's all-zero shadow bytes read
// as every button held down.
//
// TWO BITS THAT ARE NOT SIMPLY "UNPRESSED", both worth knowing before
// debugging anything odd here:
//
//  - IN0/IN1 bits 5-7 are commented in MAME as "not connected - held to
//    high", yet declared IPT_UNKNOWN with IP_ACTIVE_HIGH, which makes MAME
//    itself read them as 0. This port follows MAME's effective behaviour,
//    not the comment, because MAME's is the behaviour the game is verified
//    against. DEVNOTES.md problem #24 is the record of an unused input bit
//    being load-bearing on a different machine, so if this game ever
//    misbehaves in a way that smells like a protection check, this is the
//    first line to try flipping.
//  - IN2 bit 6 is the SOUND CPU's status line: MAME wires it to the
//    inverted bit 4 of the 8035's port 2 latch. With no sound CPU emulated,
//    that latch is always zero, so the inverted line reads 1 -- which is what
//    this file reports. It is the sound CPU's idle/ready state, so the main
//    program sees "the sound hardware is not busy", which is true here in
//    the most literal sense.
#include "dkong_input.h"

void dkong_input_update(dkong_system *system,
                        bool coin, bool start1, bool start2,
                        bool up, bool down, bool left, bool right,
                        bool jump,
                        bool rotate_button, bool mirror_button) {
    system->in0 = (uint8_t)((right ? 0x01 : 0)
                          | (left  ? 0x02 : 0)
                          | (up    ? 0x04 : 0)
                          | (down  ? 0x08 : 0)
                          | (jump  ? 0x10 : 0));

    // Player 2's cocktail controls are not wired to anything on this
    // cabinet, same as ArcadeMachine_Pacman leaves IN1's P2 joystick bits
    // inactive with the Cabinet DIP at Upright.
    system->in1 = 0x00;

    system->in2 = (uint8_t)((start1 ? 0x04 : 0)
                          | (start2 ? 0x08 : 0)
                          | (coin   ? 0x80 : 0)
                          | 0x40); // sound CPU status -- see header comment

    // Edge-detected meta controls -- a press cycles rotation or toggles the
    // mirror once, not once per frame the button is held.
    static bool prev_rotate = false, prev_mirror = false;
    if (rotate_button && !prev_rotate) system->rotation = (system->rotation + 1) & 0x03;
    if (mirror_button && !prev_mirror) system->mirror_x = !system->mirror_x;
    prev_rotate = rotate_button;
    prev_mirror = mirror_button;
}
