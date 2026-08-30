// Namco 54XX HLE implementation -- see galaga_54xx.h for scope, citations,
// and an honest statement of which parts are derived vs approximated.
#include "galaga_54xx.h"

// NO printf/Serial ANYWHERE IN THIS FILE. Two were left here from bringing
// the channel up on the host harness -- one in galaga_54xx_write() (every
// command byte from the CPU) and one in galaga_54xx_take_trigger(), which
// runs INSIDE the audio ISR, inside hal_audio_enter_critical(). On the host
// they cost nothing; on the device they block on USB CDC, and they fire
// exactly when a sound is triggered. That produced red lines on real
// hardware precisely when the player ship exploded -- the one moment this
// channel is active -- and it was invisible to the frame-budget heartbeat,
// because ISR time lands in whatever Core 0 was doing and gets charged to
// `blocked` rather than `work`.
//
// Same class of bug DEVNOTES.md problem #16 already removed once from
// Lunar Rescue's hot paths. Diagnostics for this file belong in the host
// harness (tools/galaga_host), which can print freely.

// Build with -DGALAGA_54XX_TRACE (host harness only) to log the command
// stream and envelope triggers -- that is how Galaga was confirmed to use
// only sound types A and B.
//
// __not_in_flash_func: galaga_54xx_sample() runs in the board's audio ISR
// and must never execute from flash -- same deliberate, isolated exception
// documented in ArcadeMachine_Invaders's invaders_audio.cpp and
// arcade_arduino/DEVNOTES.md problem #7.
#include "pico.h"
#ifdef GALAGA_54XX_TRACE
#include <stdio.h>
#endif

// Command table, quoted verbatim from MAME's namco54.cpp header:
//
//   0x: nop
//   1x: play sound type A
//   2x: play sound type B
//   3x: set parameters (type A) (followed by 4 bytes)
//   4x: set parameters (type B) (followed by 4 bytes)
//   5x: play sound type C
//   6x: set parameters (type C) (followed by 5 bytes)
//   7x: set volume for sound type C to x
//   8x-Fx: nop
//
// Only the high nibble selects the command. Galaga in practice issues
// `30 <4 bytes>` and `40 <4 bytes>` once during init, then alternates
// `1x`/`2x` -- traced live in the host harness. Types C are implemented as
// far as consuming their parameter bytes correctly (so the byte stream
// stays in sync if another 8080bw/Namco game ever drives this file) but
// produce no sound.
#define CMD_MASK 0xF0u

// Output samples per noise update -- a sample-and-hold, which is the piece
// that was missing and the reason this used to sound like a small balloon
// popping rather than an explosion. The real 54XX is an MCU: it can only
// rewrite its 4-bit output port every few instruction cycles, so its noise
// is a low-rate staircase, not white noise at audio rate. Holding each
// value for N output samples reproduces that, moving the bulk of the energy
// down into the 130-230Hz range the recording actually shows. 22050/16 =
// ~1.4kHz update rate.
#define NOISE_HOLD 16

void galaga_54xx_init(galaga_54xx_state *s) {
    s->last_command = 0;
    s->pending_args = 0;
    s->param_target = 0;
    s->param_index  = 0;
    for (int i = 0; i < 4; i++) { s->params_a[i] = 0; s->params_b[i] = 0; }
    s->trigger = 0;
    // Any non-zero LFSR seed works; three different ones so the channels
    // are decorrelated the way three independent chip outputs would be.
    static const uint32_t seed[2][3] = {
        { 0xACE1u, 0x1234u, 0x7FFFu }, { 0x5A5Au, 0xBEEFu, 0x0F1Eu },
    };
    for (int v = 0; v < 2; v++) {
        for (int i = 0; i < 3; i++) {
            s->voice[v].lfsr[i] = seed[v][i];
            s->voice[v].lp[i] = 0; s->voice[v].hp[i] = 0; s->voice[v].held[i] = 0;
        }
        s->voice[v].hold_ctr = 0;
        s->voice[v].env = 0;
    }
}

void galaga_54xx_write(galaga_54xx_state *s, uint8_t data) {
    s->last_command = data;
#ifdef GALAGA_54XX_TRACE
#endif

    // Mid parameter run: consume the byte rather than mistaking it for a
    // command. (An earlier stub treated EVERY byte as a command, which
    // would have read Galaga's `40 00 02 DF` parameter run as four bogus
    // commands -- including a spurious "play type B" from the 0x40.)
    if (s->pending_args > 0) {
        if (s->param_index < 4) {
            if (s->param_target == 0)      s->params_a[s->param_index] = data;
            else if (s->param_target == 1) s->params_b[s->param_index] = data;
        }
        s->param_index++;
        s->pending_args--;
        return;
    }

    switch (data & CMD_MASK) {
    case 0x10: s->trigger |= 1u; break; // play type A (layers with B)
    case 0x20: s->trigger |= 2u; break; // play type B (layers with A)
    case 0x50: break; // play type C -- unused by Galaga, no synthesis
    case 0x30: s->pending_args = 4; s->param_target = 0; s->param_index = 0; break;
    case 0x40: s->pending_args = 4; s->param_target = 1; s->param_index = 0; break;
    case 0x60: s->pending_args = 5; s->param_target = 2; s->param_index = 0; break;
    case 0x70: break; // set type C volume -- no type C output here
    default:   break; // 0x, 8x-Fx: nop
    }
}

// Per-sample envelope decay, Q16, for a burst that falls ~60dB over the
// given time at GALAGA_AUDIO_SAMPLE_RATE (22050Hz):
//     decay = exp(-ln(1000) / (seconds * 22050)) * 65536
// Type A is the shorter/tighter burst, type B the longer one. WHICH of
// Galaga's two explosion sounds is which is a guess -- the real mapping
// lives in MB8844 firmware we do not have -- as is the duration itself.
// These are the first thing to adjust by ear on hardware.
// TUNED AGAINST A REAL RECORDING of the player explosion
// (galaga_assets/samples/explosion.wav, 48kHz mono, captured from actual
// hardware). Measuring that sample rather than guessing changed all three
// of these substantially, and every earlier value here was wrong:
//
//   real duration      2.67s   (this file previously decayed in 0.36s)
//   real centroid       249Hz  (this file previously produced ~748Hz)
//   real spectrum       energy concentrated 130-230Hz, almost nothing >700Hz
//
// Decay constant for a burst falling ~60dB over `seconds`, Q16:
//     decay = exp(-ln(1000) / (seconds * 22050)) * 65536
// MEASURED: Galaga does not use types A and B as two separate effects.
// Tracing the 06XX data port live (host harness, --watch 7000..7007)
// during play shows every 54XX event is a FOUR-command burst issued in a
// single frame -- `10 10 20 20`, i.e. play-A twice then play-B twice --
// and it fires at exactly one moment: when the player's ship is destroyed
// (confirmed by rendering the frames around each burst: ship present
// before, explosion at the ship's position after). Enemy hits and every
// other effect come from the WSG, not this chip.
//
// Because the four commands land in one frame they coalesce into a single
// trigger (see galaga_54xx.h's `trigger`), and the LAST one wins -- type
// B. So type B's decay is what is actually heard, and it is the value
// matched to the recording. Type A's value only matters if a future
// caller issues a lone `1x`.
//
// Caveat on the "only one event" claim: this was traced over stage 1 plus
// three player deaths, and attract mode. Later events (challenging stage,
// boss capture, dual fighter) were not reachable by scripted input and so
// were never exercised -- if one of those turns out to use the chip, this
// note is what needs revisiting.
// Per-voice envelope decay, Q16, for a burst falling ~60dB over `seconds`:
//     decay = exp(-ln(1000) / (seconds * 22050)) * 65536
//
// WHY TWO VOICES WITH DIFFERENT DECAYS: the recording does not decay
// uniformly -- it gets BRIGHTER as it fades. Measured on
// galaga_assets/samples/explosion.wav:
//     t=0.03s  centroid 236Hz  rms 12168
//     t=0.40s  centroid 267Hz  rms  8943
//     t=1.40s  centroid 320Hz  rms  2007
//     t=2.20s  centroid 440Hz  rms   733
// i.e. the low rumble dies faster than the mid content that outlasts it.
// A single envelope cannot do that. Two layered voices can, and Galaga
// conveniently fires both types together anyway.
// NOTE the real decay is far slower than "it lasts 2.7s" suggests: the
// recording falls only ~24dB over its 2.2s of usable tail, which implies a
// ~5.5s 60dB time constant, not 2.7s. Sizing the envelope by the sample's
// LENGTH rather than its measured slope made it die about 4x too early.
// Fitted to the recording's measured slope: amplitude ~ exp(-1.3*t), i.e.
// -24dB by 2.2s. Voice 0 decays a little faster than voice 1 so the
// surviving tail is the brighter mid voice.
#define ENV_DECAY_A 65530u  // ~exp(-1.9*t), low body
#define ENV_DECAY_B 65534u  // ~exp(-0.55*t), mid tail -- deliberately much
                            // slower than the body so the survivor is the
                            // brighter voice, matching the measured drift

// Envelope is Q24, NOT Q16. `env = (env * decay) >> shift` truncates about
// half an LSB per sample; in Q16 that is a constant ~0.5/sample linear
// drain on top of the exponential, which is negligible at full level but
// completely dominates once env is small -- it dragged the tail to silence
// around 4x too early and no amount of tuning the decay constant fixed it,
// because the error is additive, not multiplicative. Q24 puts the
// truncation ~256x below the signal it is decaying.
#define ENV_ONE   (1 << 24)
#define ENV_FLOOR (ENV_ONE / 1000)  // -60dB, below audibility

// Overall level of this channel relative to the WSG voices. Empirical, and
// deliberately conservative: on the real board the 54XX explosion is
// prominent but does not swamp the music. Tune by ear alongside
// galaga_audio.cpp's OUTPUT_SCALE.
#ifndef SCALE_54XX
#define SCALE_54XX 10  // -DSCALE_54XX=0 mutes this channel, for A/B testing
                        // Chosen for headroom, not maximum loudness: at 10 an
                        // isolated burst peaks ~19.5k, so even landing on a
                        // full-scale WSG chord (~11.5k) it stays inside int16
                        // and never leans on galaga_audio.cpp's clamp --
                        // clipping would undo exactly the "fuller" quality
                        // this is tuned for. The real recording peaks ~30k but
                        // that is a normalised solo capture, not a game mix.
                        // Raise toward 14 if it sounds thin on hardware.
#endif


void galaga_54xx_take_trigger(galaga_54xx_state *s) {
    uint8_t t = s->trigger;
    if (!t) return;
    s->trigger = 0;
    // Arm each requested voice independently so A and B LAYER. Re-arming a
    // voice that is already sounding just restarts its envelope, which is
    // what a repeated command should do.
    for (int v = 0; v < 2; v++)
        if (t & (1u << v)) { s->voice[v].env = ENV_ONE; s->voice[v].hold_ctr = 0; }
#ifdef GALAGA_54XX_TRACE
#endif
}

int32_t __not_in_flash_func(galaga_54xx_sample)(galaga_54xx_state *s) {
    if (s->voice[0].env <= 0 && s->voice[1].env <= 0) return 0;

    // Band-pass corners and mixer weights computed from the literal
    // component values in MAME's galaga_a.cpp:
    //   f_hp = 1/(2*pi*(DAC_R + rIn)*c1),  f_lp = 1/(2*pi*rF*c2)
    //   DAC_R = 47k||22k||10k||4.7k = 2635 ohm
    // giving, per channel:
    //   CHANL1 (54XX out 2):  1551 Hz .. 7234 Hz
    //   CHANL2 (54XX out 1):   321 Hz .. 1592 Hz
    //   CHANL3 (54XX out 0):   104 Hz ..  723 Hz
    // as one-pole coefficients a = (1 - exp(-2*pi*f/22050)) * 65536.
    // Mixer weights are proportional to 1/R across galaga_final_mixer's
    // 33k/33k/10k, normalised to 256 -- so the LOW band dominates, which
    // is what makes it read as a "boom" rather than a hiss.
    static const int32_t A_HP[3] = { 23407,  5722,  1918 };
    static const int32_t A_LP[3] = { 57195, 23895, 12208 };
    // Mixer weights. MAME's galaga_final_mixer (33k/33k/10k) normalises to
    // {48, 48, 159}, but that assumes all three chip outputs are driven
    // equally -- which the real firmware evidently does not do for this
    // sound: the recording has almost no energy above 700Hz, i.e. the two
    // upper bands are barely present. Weighted towards the low channel to
    // match the measurement. Restore the netlist values if a future sound
    // type turns out to need the brighter bands.
    // Per-voice band weights. Voice 0 is the low-heavy body that carries
    // the initial "boom"; voice 1 is mid-weighted so that as voice 0 dies
    // away the surviving tail is brighter -- reproducing the measured
    // centroid drift from ~236Hz to ~440Hz.
    static const int32_t V_W[2][3] = {
        {   1,  14, 241 },   // voice 0: low body
        {  18, 158,  80 },   // voice 1: mid tail
    };
    static const int32_t V_LEVEL[2] = { 16, 7 };   // Q4 relative level
    static const int32_t V_DECAY[2] = { ENV_DECAY_A, ENV_DECAY_B };
    static const uint16_t V_HOLD[2] = { NOISE_HOLD, NOISE_HOLD / 2 };

    int32_t total = 0;

    for (int v = 0; v < 2; v++) {
        if (s->voice[v].env <= 0) continue;

        // Advance this voice's noise only every hold period.
        if (s->voice[v].hold_ctr == 0) {
            for (int c = 0; c < 3; c++) {
                // 16-bit maximal-length LFSR, 4 bits at a time to match the
                // width of the real chip's DAC input.
                uint32_t l = s->voice[v].lfsr[c];
                l = (l >> 1) ^ (uint32_t)((-(int32_t)(l & 1u)) & 0xB400u);
                s->voice[v].lfsr[c] = l;
                s->voice[v].held[c] = (((int32_t)(l & 0x0Fu)) - 8) << 8;
            }
        }
        if (++s->voice[v].hold_ctr >= V_HOLD[v]) s->voice[v].hold_ctr = 0;

        int32_t mix = 0;
        for (int c = 0; c < 3; c++) {
            int32_t x = s->voice[v].held[c];
            s->voice[v].lp[c] += ((x - s->voice[v].lp[c]) * A_LP[c]) >> 16;
            s->voice[v].hp[c] += ((s->voice[v].lp[c] - s->voice[v].hp[c]) * A_HP[c]) >> 16;
            int32_t band = s->voice[v].lp[c] - s->voice[v].hp[c];
            mix += (band * V_W[v][c]) >> 8;
        }

        mix = (int32_t)(((int64_t)mix * s->voice[v].env) >> 24);
        total += (mix * V_LEVEL[v]) >> 4;

        s->voice[v].env = (int32_t)(((int64_t)s->voice[v].env * V_DECAY[v]) >> 16);
        if (s->voice[v].env < ENV_FLOOR) s->voice[v].env = 0;
    }

    return total * SCALE_54XX;
}
