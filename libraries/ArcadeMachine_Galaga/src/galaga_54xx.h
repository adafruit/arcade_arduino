// Namco 54XX HLE -- explosion/noise channel.
//
// The real 54XX is a Fujitsu MB8844 MCU programmed as a noise generator
// (MAME's namco54.cpp header: "used for explosions, the shoot sound in
// Bosconian, and the tire screech sound in Pole Position"). It has three
// 4-bit output channels; on Galaga each drives a resistor-ladder DAC into
// its own op-amp band-pass filter, and the three are summed by an op-amp
// mixer (MAME's galaga_a.cpp discrete netlist).
//
// WHY THIS IS AN APPROXIMATION, and what part of it is not:
//
// Modern MAME emulates this chip at LOW level -- it runs the actual MB8844
// firmware and feeds its output into the discrete netlist. We have no
// firmware dump, so the noise *generator* here cannot be a port of the real
// thing; it is an approximation of documented behaviour, exactly as
// galaga_51xx.h describes for the 51XX, and the envelope shape in
// particular is a guess to be tuned by ear on hardware.
//
// What IS derived from real, cited data rather than invented:
//  - The command protocol, quoted verbatim from namco54.cpp's header table
//    (see galaga_54xx.cpp). Galaga uses only types A and B -- confirmed
//    empirically by tracing the actual command stream during play in the
//    host harness, not assumed: it issues 3x/4x to set parameters once,
//    then alternates 1x/2x to fire. Types C (5x/6x/7x) never appear.
//  - The three filter bands and the mixer weights, computed from the
//    literal resistor/capacitor values in MAME's galaga_a.cpp
//    (galaga_chanl1/2/3_filt and galaga_final_mixer). See galaga_54xx.cpp
//    for the arithmetic.
#ifndef GALAGA_54XX_H
#define GALAGA_54XX_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // --- command protocol state (written from the CPU side) ---
    uint8_t last_command;
    uint8_t pending_args;  // parameter bytes still expected after a 3x/4x/6x
    uint8_t param_target;  // 0 = type A, 1 = type B, 2 = type C
    uint8_t param_index;   // position within the current parameter run
    uint8_t params_a[4];
    uint8_t params_b[4];

    // Pending play commands as a BITMASK: bit0 = type A, bit1 = type B.
    // A mask rather than a single slot because Galaga fires both types
    // together (`10 10 20 20` in one frame, see the .cpp) and they must
    // LAYER -- an earlier single-slot version let the last command win,
    // which threw away half the sound. Handed over under
    // hal_audio_enter_critical() (see galaga_audio.cpp), since the CPU
    // sets it and the audio ISR consumes it.
    volatile uint8_t trigger;

    // --- synthesis state (owned by the audio ISR after handover) ---
    // One independent voice per sound type, summed. The real chip has three
    // separate output channels, so layering is closer to the hardware than
    // picking one sound; it is also what reproduces the recording's
    // characteristic brightening as it decays (see the .cpp).
    struct {
        uint32_t lfsr[3];      // one noise source per band
        int32_t  held[3];      // sample-and-held noise value per band
        uint16_t hold_ctr;     // output samples since the last noise update
        int32_t  lp[3], hp[3]; // per-band filter state
        int32_t  env;          // Q24 envelope level, 0 = silent (see .cpp:
                               // Q16 was not enough -- truncation killed the tail)
    } voice[2];
} galaga_54xx_state;

void galaga_54xx_init(galaga_54xx_state *s);

// Called from the 06XX mux (galaga_ports.cpp) when chip-select 3 is active
// and a write happens in write mode.
void galaga_54xx_write(galaga_54xx_state *s, uint8_t data);

// Reads and clears any pending play command, starting its envelope. Call
// from the audio fill callback inside hal_audio_enter_critical().
void galaga_54xx_take_trigger(galaga_54xx_state *s);

// One mixed sample of the explosion channel, or 0 when idle. Called
// per-sample from the audio fill callback.
int32_t galaga_54xx_sample(galaga_54xx_state *s);

#ifdef __cplusplus
}
#endif

#endif
