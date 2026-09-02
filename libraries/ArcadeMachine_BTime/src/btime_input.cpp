// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Burger Time input mapping -- bit layouts verified against
// INPUT_PORTS_START( btime ) (src/mame/dataeast/btime.cpp:1263):
//
//   P1     (0x4000): bit0=right bit1=left bit2=up bit3=down  (4-WAY)
//                    bit4=BUTTON1 (pepper)
//                    bit5=IPT_UNKNOWN, bits6-7=IPT_UNUSED
//   P2     (0x4001): same layout, cocktail player 2
//   SYSTEM (0x4002): bit0=start1 bit1=start2 bit2=tilt
//                    bit3=IPT_UNKNOWN, bits4-5=IPT_UNUSED
//                    bit6=COIN1 bit7=COIN2
//
// EVERYTHING IS ACTIVE LOW -- EXCEPT THE TWO COIN BITS. Look at the port
// definition: every joystick and button line is IP_ACTIVE_LOW, and then
//
//   PORT_BIT( 0x40, IP_ACTIVE_HIGH, IPT_COIN1 ) PORT_CHANGED_MEMBER(...)
//   PORT_BIT( 0x80, IP_ACTIVE_HIGH, IPT_COIN2 ) PORT_CHANGED_MEMBER(...)
//
// two ACTIVE HIGH bits in the middle of an active-low port. This is exactly
// the kind of detail that gets "tidied" into consistency by accident, so it
// is spelled out here: the idle SYSTEM byte is 0xFF with the coin bits
// CLEARED, i.e. 0x3F, and inserting a coin SETS bit 6.
//
// The coin bits are also the main CPU's only interrupt source on this
// board. There is no vblank interrupt; see btime_machine.h.
#include "btime_input.h"
#include "btime_ports.h"

void btime_input_update(btime_system *system,
                        bool coin, bool start1, bool start2,
                        bool up, bool down, bool left, bool right,
                        bool pepper,
                        bool rotate_button, bool mirror_button) {
    // Active low: start from all-ones and clear the pressed bits. Bits 5-7
    // of P1/P2 are UNKNOWN/UNUSED and left at 1, which is their released
    // state for an active-low line -- deliberately not forced to 0. See
    // DEVNOTES.md problem #24: an unused input bit reading the wrong
    // constant has already made a machine in this project self-reset.
    system->p1 = (uint8_t)(0xFF
                           & (right  ? ~0x01 : 0xFF)
                           & (left   ? ~0x02 : 0xFF)
                           & (up     ? ~0x04 : 0xFF)
                           & (down   ? ~0x08 : 0xFF)
                           & (pepper ? ~0x10 : 0xFF));

    // Player 2's cocktail controls are not wired to anything on this
    // cabinet, and the Cabinet DIP is left at Upright -- same as
    // ArcadeMachine_Pacman and _DKong leave their P2 bits idle.
    system->p2 = 0xFF;

    // SYSTEM: active low for start/tilt, ACTIVE HIGH for the two coins.
    system->system_in = (uint8_t)((0xFF
                                   & (start1 ? ~0x01 : 0xFF)
                                   & (start2 ? ~0x02 : 0xFF)
                                   & ~0xC0u)          // coins idle low
                                  | (coin ? 0x40 : 0x00));

    // The coin line is also the interrupt. MAME's
    // coin_inserted_irq_hi() fires on the CHANGE to a set value:
    //     if (newval) m_maincpu->set_input_line(0, HOLD_LINE);
    // so this is an edge, not a level -- holding the coin button must not
    // re-trigger the interrupt every frame.
    static bool prev_coin = false;
    if (coin && !prev_coin) btime_ports_coin_irq(system);
    prev_coin = coin;

    // Edge-detected meta controls -- a press cycles rotation or toggles the
    // mirror once, not once per frame the button is held.
    static bool prev_rotate = false, prev_mirror = false;
    if (rotate_button && !prev_rotate) system->rotation = (uint8_t)((system->rotation + 1) & 0x03);
    if (mirror_button && !prev_mirror) system->mirror_x = !system->mirror_x;
    prev_rotate = rotate_button;
    prev_mirror = mirror_button;
}
