// Namco 51XX HLE (high-level emulation) -- coin/credit/joystick I/O chip,
// reached from the main Z80 through the 06XX mux (see galaga_ports.cpp).
//
// This is a hand-coded reimplementation of the chip's behavior, not a port
// of real firmware: in current MAME (src/mame/namco/namco51.cpp, checked
// directly against the source) the namco_51xx_device is a real emulated
// Fujitsu MB8843 MCU core running a dumped 1KB program (51xx.bin) -- this
// project has no such dump, and per the standing HLE-over-LLE decision
// (see project memory) isn't adding a 4th CPU-core architecture just to
// run it even if it did. The command byte meanings (write side) still come
// from namco51.cpp's own header comment, a stable documented fact
// regardless of implementation.
//
// The READ-side response model below, however, is corrected from an
// earlier Phase A pass that guessed at it from MAME's input_callback<0..3>
// wiring (a round-robin over 4 raw input nibbles) -- that guess was WRONG,
// found by cross-referencing a real working reference implementation:
// danjulio/gcore_galagino (fork of harbaum/galagino,
// https://github.com/danjulio/gcore_galagino/blob/master/galagino/
// emulation.c), an ESP32 port that's confirmed to actually boot and run
// real Galaga. That project's author reverse-engineered the real protocol
// (their code comments cite line numbers from an actual disassembled ROM
// listing, e.g. "game_ctrl.s L1024", that this project doesn't have) --
// this is empirical/behavioral evidence from a working implementation, not
// a MAME source citation, and is cited as such rather than implied to be
// MAME-equivalent. Real shape: a `namco_cnt` counter, reset to 0 on every
// 06XX control-register (0x7100) write (see galaga_ports.cpp), indexes a
// 3-entry response per read: in NON-credit mode, all 3 entries are 0xFF
// (galaga doesn't use the button-mapping bytes outside credit mode, per
// that project's own comment); in credit mode, entry 0 is the credit count
// in BCD, entries 1-2 are direction/fire bits. `namco_cnt` increments on
// both reads AND writes (mirroring that project's exact behavior); reads
// past index 2 return 0xFF.
#ifndef GALAGA_51XX_H
#define GALAGA_51XX_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // Command byte protocol state (galaga_51xx.cpp's write()):
    //   0x00      nop
    //   0x01+4    set coinage (4 argument bytes follow; consumed, not
    //             yet acted on)
    //   0x02      enter credit mode, enable start buttons
    //   0x03      disable joystick remap
    //   0x04      enable joystick remap
    //   0x05      enter switch mode (leaves credit mode)
    //   0x06-0x07 nop
    uint8_t pending_args;   // remaining coinage-arg bytes still expected
    bool    credit_mode;    // set by command 0x02, cleared by 0x05 -- also
                             // cleared by a misclatch 0x6823 write (see
                             // galaga_ports.cpp), matching the reference
                             // implementation's "this also resets the
                             // 51xx" comment on that same write.
    bool    joystick_remap; // set/cleared by commands 0x03/0x04 -- tracked
                             // but not yet consumed by anything (Galaga's
                             // 2-way stick doesn't need rotary remapping
                             // the way Xevious's 8-way one would).
    uint8_t namco_cnt;      // read/write position since the last 06XX
                             // control-register write -- reset externally
                             // by galaga_ports.cpp's 0x7100 write handler,
                             // NOT by this file.

    // Credit count, 0-99, reported to the game as BCD in credit mode.
    // The real 51XX is "an I/O device with built-in coin management"
    // (MAME namco51.cpp's own header), so coin/start bookkeeping lives
    // HERE rather than in the sketch or the machine: the game never sees
    // raw coin/start button bits, only the resulting credit count.
    uint8_t credit;

    // Per-player control bytes reported in credit mode (bytes 1 and 2 of
    // the 3-byte response). Active LOW, layout `..FLURD`:
    //   bit4 fire, bit3 left, bit2 up, bit1 right, bit0 down
    // Galaga's cabinet is a 2-way stick, so up/down stay set (released).
    // Refreshed once per frame by galaga_51xx_set_inputs().
    uint8_t p1_ctrl;
    uint8_t p2_ctrl;

    // Edge-detection state for coin/start/fire.
    //
    // FIRE IS A ONE-SHOT PULSE, NOT A LEVEL -- this is the single most
    // important thing about this struct, and getting it wrong is a real bug
    // that shipped here once (see the .cpp). Galaga's ROM does not
    // edge-detect the fire button itself: it fires one bullet for every
    // frame in which it reads the bit as set. Reporting the raw button
    // level therefore fires a bullet per frame for as long as the button is
    // held, which the 2-bullets-on-screen cap turns into "every press fires
    // exactly two shots". The real 51XX evidently pulses instead, so that
    // is what this models.
    //
    // fire_pulse is set on the press edge and cleared only once the game
    // has actually READ the control byte carrying it (see
    // galaga_51xx_read). That read-confirmation is deliberately used
    // instead of a fixed frame count: it guarantees the game samples
    // exactly one asserted frame per press, however often it happens to
    // poll -- which is what the reference implementation's fixed hold was
    // groping at with its comment "0 is too short for score enter, 5 is
    // too long".
    bool    prev_coin;
    bool    prev_start1;
    bool    prev_start2;
    bool    prev_fire;
    bool    fire_pulse;
} galaga_51xx_state;

void galaga_51xx_init(galaga_51xx_state *s);

// Called from the 06XX mux (galaga_ports.cpp) when chip-select 0 is active
// and a write happens in write mode.
void galaga_51xx_write(galaga_51xx_state *s, uint8_t data);

// Called from the 06XX mux when chip-select 0 is active and a read happens
// in read mode.
uint8_t galaga_51xx_read(galaga_51xx_state *s);

// Feeds the chip this frame's button state (active HIGH: true = pressed).
// Call exactly once per frame, before the frame's CPU cycles run --
// galaga_input.cpp does this. Handles coin/start edge detection and credit
// bookkeeping internally, matching the real chip's built-in coin
// management; the game reads the resulting credit count, never the raw
// coin/start bits.
void galaga_51xx_set_inputs(galaga_51xx_state *s,
                            bool coin, bool start1, bool start2,
                            bool left, bool right, bool fire);

#ifdef __cplusplus
}
#endif

#endif
