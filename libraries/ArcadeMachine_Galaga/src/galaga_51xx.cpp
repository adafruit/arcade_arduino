// Namco 51XX HLE implementation -- see galaga_51xx.h for the full
// protocol citation (command bytes from MAME's namco51.cpp header
// comment; read-side response model corrected against a working
// reference implementation, danjulio/gcore_galagino's emulation.c).
#include "galaga_51xx.h"

void galaga_51xx_init(galaga_51xx_state *s) {
    s->pending_args   = 0;
    s->credit_mode    = false;
    s->joystick_remap = false;
    s->namco_cnt      = 0;
    s->credit         = 0;
    s->p1_ctrl        = 0xFF; // active-low, idle = nothing pressed
    s->p2_ctrl        = 0xFF;
    s->prev_coin      = false;
    s->prev_start1    = false;
    s->prev_start2    = false;
    s->prev_fire      = false;
    s->fire_pulse     = false;
}

// Coin/credit bookkeeping + control-byte assembly. Modelled on
// danjulio/gcore_galagino's emulation.c (a confirmed-booting Galaga), since
// modern MAME's namco51 is a full LLE -- it runs the real MB8843 firmware
// and its read() just returns the MCU's output port, so there is no HLE
// protocol there to copy. See galaga_51xx.h's citation note.
void galaga_51xx_set_inputs(galaga_51xx_state *s,
                            bool coin, bool start1, bool start2,
                            bool left, bool right, bool fire) {
    // Coin on the press edge only -- a held coin button must not run the
    // credit count up every frame.
    if (coin && !s->prev_coin && s->credit < 99) s->credit++;

    // Start consumes a credit on the press edge. The game notices the
    // credit count change and begins play; it is never shown the start
    // button itself. Player 2's start costs a credit the same way.
    if (start1 && !s->prev_start1 && s->credit > 0) s->credit--;
    if (start2 && !s->prev_start2 && s->credit > 0) s->credit--;

    s->prev_coin   = coin;
    s->prev_start1 = start1;
    s->prev_start2 = start2;

    // `..FLURD`, active low. Galaga's stick is 2-way so up/down stay set.
    uint8_t p1 = 0xFF;
    if (left)  p1 = (uint8_t)(p1 & ~0x08u);
    if (right) p1 = (uint8_t)(p1 & ~0x02u);

    // Fire: a ONE-SHOT PULSE on the press edge, held only until the game
    // has read it. See galaga_51xx.h's fire_pulse comment for why reporting
    // the raw level is wrong (it fires a bullet per frame, which the
    // 2-bullet cap turns into a permanent double shot).
    if (fire && !s->prev_fire) s->fire_pulse = true;
    s->prev_fire = fire;
    if (s->fire_pulse) p1 = (uint8_t)(p1 & ~0x10u);

    s->p1_ctrl = p1;
    s->p2_ctrl = 0xFF; // cocktail/P2 stick not wired on this cabinet
}

void galaga_51xx_write(galaga_51xx_state *s, uint8_t data) {
    if (s->pending_args > 0) {
        // Mid "set coinage" command -- consume the argument byte and
        // ignore it (see galaga_51xx.h's known-gap note on coin/credit
        // counting).
        s->pending_args--;
        s->namco_cnt++;
        return;
    }

    switch (data) {
    case 0x00: case 0x06: case 0x07:
        break; // nop
    case 0x01:
        s->pending_args = 4; // 4 coinage argument bytes follow
        break;
    case 0x02:
        s->credit_mode = true;
        break;
    case 0x03:
        s->joystick_remap = false;
        break;
    case 0x04:
        s->joystick_remap = true;
        break;
    case 0x05:
        s->credit_mode = false; // "switch mode" -- leaves credit mode
        break;
    default:
        break; // undocumented byte -- ignore rather than misbehave
    }
    s->namco_cnt++;
}

uint8_t galaga_51xx_read(galaga_51xx_state *s) {
    uint8_t retval;

    if (!s->credit_mode) {
        // Galaga doesn't use the button-mapping bytes outside credit
        // mode -- confirmed against the reference implementation's own
        // comment ("galaga doesn't seem to use the button mappings...
        // in non-credit mode"). Always 0xFF here.
        retval = 0xFF;
    } else {
        // 3-byte response: credit count in BCD, then each player's
        // control byte (see galaga_51xx.h for the bit layout).
        uint8_t mapb1[3];
        mapb1[0] = (uint8_t)(16u * (s->credit / 10u) + (s->credit % 10u)); // BCD
        mapb1[1] = s->p1_ctrl;
        mapb1[2] = s->p2_ctrl;
        retval = (s->namco_cnt > 2) ? 0xFF : mapb1[s->namco_cnt];
        // Byte 1 is player 1's control byte -- the moment the game actually
        // consumes a fire pulse, so retire it here rather than after a
        // fixed number of frames (see galaga_51xx.h).
        if (s->namco_cnt == 1) s->fire_pulse = false;
    }

    s->namco_cnt++;
    return retval;
}
