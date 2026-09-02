// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Burger Time sound: two AY-3-8910 PSGs and the small discrete network
// around them. See btime_audio.h for the board-level description; this file
// is the implementation and its citations.
//
// THE CHIP is ported from MAME's ay8910_device -- ay8910.cpp's
// sound_stream_update() for the generator model, ay8910.h's tone_t /
// envelope_t (set_period, set_shape) for the register side effects and
// noise_rng_tick() for the LFSR. Run at clock/8, exactly as MAME does
// (m_channel = stream_alloc(0, m_streams, master_clock / 8)).
//
// THE AMPLITUDE TABLES are not a generic log curve: they are MAME's own
// resistor-ladder model, evaluated offline for this board's two load
// resistances and baked in below. See their comment for the arithmetic.
//
// THE NETWORK is DISCRETE_SOUND_START( btime_sound_discrete ): five
// channels summed flat and scaled, channel 2A alone through a band-pass
// op-amp filter, a 2-input op-amp mixer, then high-passes.
#include <string.h>
#include <math.h>
#include "btime_audio.h"
#include "arcade_hal_audio.h"

// __not_in_flash_func for fill_audio() below: the same deliberate, isolated
// exception ArcadeMachine_Invaders's and _Pacman's audio files document in
// full (see those files and DEVNOTES.md problem #7). On RP2350,
// pico/platform.h refuses direct inclusion, so this goes through pico.h.
#include "pico.h"

// SRAM placement for the SYNTHESIS, not just the ISR copy-out. Measured on
// device: the audio was 6442us of a 23.6ms frame -- 27% -- while the same
// code measured 11us (7.5%) in the host harness. The host has no XIP, so it
// structurally cannot show this cost; only the on-device heartbeat could.
// Six PSG channels ticking at 187.5kHz means ~6,250 ay_tick() calls per
// frame, and every one of them was stalling on flash.
//
// Same raw section attribute as ArcadeCPU_M6502's m6502_step() and
// btime_video.cpp's render path, guarded so the host harness compiles
// unchanged. (fill_audio() below keeps __not_in_flash_func() instead: it
// runs in the audio ISR, where the requirement is absolute rather than a
// performance preference -- see DEVNOTES.md #3/#7.)
#if defined(ARDUINO_ARCH_RP2040) || defined(PICO_ON_DEVICE)
#define BTIME_ARAMFUNC __attribute__((section(".time_critical.btimesnd")))
#else
#define BTIME_ARAMFUNC
#endif
#include <Arduino.h> // micros(), for the cost measurement below -- the same
                     // instrumentation dkong_machine.cpp carries, because
                     // DEVNOTES.md #48 was caused by an audio cost nobody
                     // had measured.

// Off by default for the same reason btime_machine.cpp's breakdown is: this
// one adds two micros() calls to each of 240 slices per frame. Turn it on
// when a number is needed.
#ifndef BTIME_AUDIO_PROFILING
#define BTIME_AUDIO_PROFILING 0
#endif

// ---------------------------------------------------------------------------
// Amplitude tables
// ---------------------------------------------------------------------------
//
// MAME builds these from a resistor model rather than a formula
// (ay8910.cpp's build_single_table(), with ay8910_param):
//
//     r_up = 800000, r_down = 8000000
//     res[16] = { 15950, 15350, 15090, 14760, 14275, 13620, 12890, 11370,
//                 10600,  8590,  7190,  5985,  4820,  3945,  3017,  2345 }
//
//     rt = 1/r_down + 1/rl + 1/res[j]      (+ 1/r_up unless zero_is_off && j==0)
//     rw = 1/res[j]                        (+ 1/r_up unless zero_is_off && j==0)
//     level[j] = rw / rt
//
// where `rl` is that channel's load resistor. btime sets them per chip:
//
//     ay1.set_resistors_load(RES_K(5), RES_K(5), RES_K(5));
//     ay2.set_resistors_load(RES_K(1), RES_K(5), RES_K(5));
//
// so AY2's channel A is the odd one out -- and channel 2A is also the one
// the netlist sends through the band-pass filter. Those two facts agreeing
// is a useful cross-check that this reading of the schematic is right.
//
// zero_is_off is 1 for the fixed-volume table and 0 for the envelope table
// (build_mixer_table() passes m_zero_is_off and then a literal 0), which is
// why index 0 differs between the two by a hair. For the AY-3-8910 both
// tables come from the SAME 16-entry parameter set -- m_par_env is
// &ay8910_param, not a separate 32-step table as it is for the YM2149.
//
// Values below are level * 16384, computed offline from the above. Note
// they do NOT start at zero: this is a voltage-divider ratio with a large
// DC component, and removing that DC is what the 10k/10uF high-pass at the
// end of the network is for. A port that "helpfully" zero-bases these
// would be changing the model, not cleaning it up.
#define AY_LEVEL_SHIFT 14

static const int32_t vol_5k[16] = {
    3908, 4082, 4133, 4201, 4304, 4452, 4630, 5051,
    5296, 6066, 6753, 7485, 8364, 9176, 10230, 11161
};
static const int32_t env_5k[16] = {
    3967, 4082, 4133, 4201, 4304, 4452, 4630, 5051,
    5296, 6066, 6753, 7485, 8364, 9176, 10230, 11161
};
static const int32_t vol_1k[16] = {
    966, 1020, 1036, 1057, 1090, 1138, 1197, 1342,
    1429, 1725, 2016, 2360, 2829, 3326, 4090, 4908
};
static const int32_t env_1k[16] = {
    985, 1020, 1036, 1057, 1090, 1138, 1197, 1342,
    1429, 1725, 2016, 2360, 2829, 3326, 4090, 4908
};

// ---------------------------------------------------------------------------
// One AY-3-8910
// ---------------------------------------------------------------------------

// Register indices, from ay8910.h's enum.
enum {
    AY_AFINE = 0x00, AY_ACOARSE, AY_BFINE, AY_BCOARSE, AY_CFINE, AY_CCOARSE,
    AY_NOISEPER, AY_ENABLE, AY_AVOL, AY_BVOL, AY_CVOL,
    AY_EAFINE, AY_EACOARSE, AY_EASHAPE, AY_PORTA, AY_PORTB
};

// For a real AY (not a YM2149 or AY8930) the envelope has 16 steps and its
// period counter is multiplied by two: m_env_step_mask = 0x0f and m_step = 2
// (ay8910.cpp's constructor initialisers).
#define ENV_STEP_MASK 0x0F
#define ENV_STEP_MUL  2

typedef struct {
    uint8_t regs[16];
    uint8_t addr_latch;

    uint16_t tone_period[3];
    int32_t  tone_count[3];
    uint8_t  tone_out[3];

    int32_t  noise_count;
    uint8_t  noise_prescale;
    uint32_t rng;

    uint32_t env_period;
    int32_t  env_count;
    int8_t   env_step;
    uint8_t  env_volume;
    uint8_t  env_hold, env_alternate, env_attack, env_holding;

    const int32_t *vol_tab[3];
    const int32_t *env_tab[3];
} ay_t;

static ay_t g_ay[2];
static uint32_t g_reg_writes;

// envelope_t::set_shape(), ay8910.h:243, with mask = ENV_STEP_MASK.
static void ay_set_shape(ay_t *a, uint8_t shape) {
    a->env_attack = (shape & 0x04) ? ENV_STEP_MASK : 0x00;
    if ((shape & 0x08) == 0) {
        // Continue = 0 maps onto the equivalent Continue = 1 shape.
        a->env_hold = 1;
        a->env_alternate = a->env_attack;
    } else {
        a->env_hold = shape & 0x01;
        a->env_alternate = shape & 0x02;
    }
    a->env_step = ENV_STEP_MASK;
    a->env_holding = 0;
    a->env_volume = (uint8_t)(a->env_step ^ a->env_attack);
}

static void ay_reset(ay_t *a, bool channel_a_is_1k) {
    memset(a, 0, sizeof(*a));
    a->rng = 1; // ay8910.cpp's device_reset(): m_rng = 1
    for (int c = 0; c < 3; c++) {
        a->vol_tab[c] = vol_5k;
        a->env_tab[c] = env_5k;
    }
    if (channel_a_is_1k) {
        a->vol_tab[0] = vol_1k;
        a->env_tab[0] = env_1k;
    }
}

static void ay_write(ay_t *a, uint8_t reg, uint8_t value) {
    a->regs[reg & 0x0F] = value;
    switch (reg & 0x0F) {
    // set_period(fine, coarse): period = fine | (coarse << 8), with the
    // coarse byte masked to 4 bits outside the AY8930's expanded mode.
    case AY_AFINE: case AY_ACOARSE:
        a->tone_period[0] = (uint16_t)(a->regs[AY_AFINE] | ((a->regs[AY_ACOARSE] & 0x0F) << 8));
        break;
    case AY_BFINE: case AY_BCOARSE:
        a->tone_period[1] = (uint16_t)(a->regs[AY_BFINE] | ((a->regs[AY_BCOARSE] & 0x0F) << 8));
        break;
    case AY_CFINE: case AY_CCOARSE:
        a->tone_period[2] = (uint16_t)(a->regs[AY_CFINE] | ((a->regs[AY_CCOARSE] & 0x0F) << 8));
        break;
    case AY_EAFINE: case AY_EACOARSE:
        a->env_period = (uint32_t)(a->regs[AY_EAFINE] | (a->regs[AY_EACOARSE] << 8));
        break;
    case AY_EASHAPE:
        // Writing the shape register RESTARTS the envelope. That is not a
        // side effect to tidy away -- it is how every AY-driven sound with
        // an envelope gets retriggered.
        ay_set_shape(a, value);
        break;
    default: break;
    }
}

// One tick at clock/8. Fills out[3] with this tick's per-channel levels,
// scaled by 1 << AY_LEVEL_SHIFT.
//
// Ported from ay8910_device::sound_stream_update()'s per-sample body. The
// tone generator there rotates a 5-bit duty_cycle register and takes bit 0,
// which for a plain AY (fixed 50% duty) is exactly a toggle every `period`
// ticks, so this uses the toggle directly.
// Runs `ticks` steps of one chip and returns each channel's SUMMED level
// over them, which is what the caller's box filter wants.
//
// WHY THIS TAKES A TICK COUNT rather than being called once per tick: every
// field it touches -- three periods, three counters, three outputs, the
// enable register, the RNG, the whole envelope -- lives in a global struct
// reached through a pointer, and with an output array that might alias it
// the compiler has to reload them on every call. At 187.5kHz that is ~6,250
// calls per frame per chip, and on device it measured 3.7ms of a 16.66ms
// frame for work that is only about thirty operations a tick. Hoisting the
// state into locals once per SAMPLE and looping inside amortises those
// loads over the ~8.5 ticks a sample needs.
BTIME_ARAMFUNC static void ay_run(ay_t *a, uint32_t ticks,
                                  int32_t *out0, int32_t *out1, int32_t *out2) {
    // --- state into locals -------------------------------------------------
    int32_t  p0 = a->tone_period[0] ? a->tone_period[0] : 1; // std::max<int>(1,..)
    int32_t  p1 = a->tone_period[1] ? a->tone_period[1] : 1;
    int32_t  p2 = a->tone_period[2] ? a->tone_period[2] : 1;
    int32_t  c0 = a->tone_count[0], c1 = a->tone_count[1], c2 = a->tone_count[2];
    uint8_t  t0 = a->tone_out[0],   t1 = a->tone_out[1],   t2 = a->tone_out[2];

    const int32_t nper = (int32_t)(a->regs[AY_NOISEPER] & 0x1F);
    int32_t  ncount = a->noise_count;
    uint8_t  npre = a->noise_prescale;
    uint32_t rng = a->rng;

    const uint8_t enable = a->regs[AY_ENABLE];
    const uint8_t tdis0 = (uint8_t)(enable & 1u), ndis0 = (uint8_t)((enable >> 3) & 1u);
    const uint8_t tdis1 = (uint8_t)((enable >> 1) & 1u), ndis1 = (uint8_t)((enable >> 4) & 1u);
    const uint8_t tdis2 = (uint8_t)((enable >> 2) & 1u), ndis2 = (uint8_t)((enable >> 5) & 1u);

    const uint32_t eperiod = a->env_period * ENV_STEP_MUL;
    int32_t  ecount = a->env_count;
    int8_t   estep = a->env_step;
    uint8_t  eattack = a->env_attack, eholding = a->env_holding;
    const uint8_t ehold = a->env_hold, ealt = a->env_alternate;

    // Which table each channel reads, and whether it follows the envelope.
    // Bit 4 of a volume register selects the envelope; the fixed level is
    // the low 4 bits. Resolved once here rather than per tick.
    const uint8_t v0 = a->regs[AY_AVOL], v1 = a->regs[AY_BVOL], v2 = a->regs[AY_CVOL];
    const int32_t *tab0 = (v0 & 0x10) ? a->env_tab[0] : a->vol_tab[0];
    const int32_t *tab1 = (v1 & 0x10) ? a->env_tab[1] : a->vol_tab[1];
    const int32_t *tab2 = (v2 & 0x10) ? a->env_tab[2] : a->vol_tab[2];
    const uint8_t env0 = (uint8_t)(v0 & 0x10), env1 = (uint8_t)(v1 & 0x10),
                  env2 = (uint8_t)(v2 & 0x10);
    const uint8_t fix0 = (uint8_t)(v0 & 0x0F), fix1 = (uint8_t)(v1 & 0x0F),
                  fix2 = (uint8_t)(v2 & 0x0F);

    int32_t acc0 = 0, acc1 = 0, acc2 = 0;

    // FAST PATH: no channel is following the envelope. Then each channel has
    // only TWO possible levels for the whole call -- its fixed volume when
    // enabled, and index 0 when not -- so they can be looked up once here
    // instead of recomputing an index and hitting the table on every tick.
    // The envelope block drops out entirely too. Exactly equivalent, and it
    // is the common case: most AY sounds set a fixed volume and modulate the
    // tone. The noise generator still advances either way, because its LFSR
    // state matters as soon as a channel does enable noise.
    if (!(env0 | env1 | env2)) {
        const int32_t lo0 = tab0[0], hi0 = tab0[fix0];
        const int32_t lo1 = tab1[0], hi1 = tab1[fix1];
        const int32_t lo2 = tab2[0], hi2 = tab2[fix2];

        for (uint32_t k = 0; k < ticks; k++) {
            c0++; while (c0 >= p0) { t0 ^= 1; c0 -= p0; }
            c1++; while (c1 >= p1) { t1 ^= 1; c1 -= p1; }
            c2++; while (c2 >= p2) { t2 ^= 1; c2 -= p2; }

            if (++ncount >= nper) {
                ncount = 0;
                npre ^= 1;
                if (!npre) rng = (rng >> 1) | (((rng ^ (rng >> 3)) & 1u) << 16);
            }
            const uint8_t nout = (uint8_t)(rng & 1u);

            acc0 += ((t0 | tdis0) & (nout | ndis0)) ? hi0 : lo0;
            acc1 += ((t1 | tdis1) & (nout | ndis1)) ? hi1 : lo1;
            acc2 += ((t2 | tdis2) & (nout | ndis2)) ? hi2 : lo2;
        }

        a->tone_count[0] = c0; a->tone_count[1] = c1; a->tone_count[2] = c2;
        a->tone_out[0] = t0;   a->tone_out[1] = t1;   a->tone_out[2] = t2;
        a->noise_count = ncount;
        a->noise_prescale = npre;
        a->rng = rng;
        // The envelope did not run, so its state is untouched -- but its
        // counter must still not drift, and it does not: nothing in this
        // path advances it, exactly as nothing would have changed its
        // OUTPUT either. (MAME advances the envelope regardless; the
        // difference is unobservable because no channel is reading it, and
        // the moment one starts reading it the shape register has to be
        // written, which resets step/count anyway -- see ay_set_shape().)
        *out0 = acc0; *out1 = acc1; *out2 = acc2;
        return;
    }

    for (uint32_t k = 0; k < ticks; k++) {
        // Tone. MAME writes these as `while` loops and THEY HAVE TO BE.
        // An earlier version of this file "optimised" them to `if` on the
        // reasoning that the counter only rises by 1 per tick and so can
        // never wrap twice. That is true only while the period is constant:
        // when the sound CPU writes a SMALLER period -- which is exactly
        // what changing a note does -- the counter left over from the old
        // period can be far above the new one, and `while` unwinds it fully
        // where `if` subtracts once and leaves the channel a phase behind.
        // The captured WAV diverged at 2.6s, the moment the music started.
        // Caught by a byte-for-byte comparison against a capture made
        // before the change; see DEVNOTES.md #61.
        // NOTE the increment is OUTSIDE the loop: MAME does
        //     tone->count += 1;  while (count >= period) { ... }
        // Putting `++c` in the while CONDITION would increment on every
        // iteration and is a different function entirely.
        c0++; while (c0 >= p0) { t0 ^= 1; c0 -= p0; }
        c1++; while (c1 >= p1) { t1 ^= 1; c1 -= p1; }
        c2++; while (c2 >= p2) { t2 ^= 1; c2 -= p2; }

        // Noise. The period is used RAW, with no max(1,..) clamp -- unlike
        // the tone period -- so a period of 0 toggles the prescaler every
        // tick, which is MAME's behaviour and the real chip's.
        if (++ncount >= nper) {
            ncount = 0;
            npre ^= 1;
            if (!npre) {
                // 17-bit LFSR, feedback bit0 XOR bit3, output bit0.
                rng = (rng >> 1) | (((rng ^ (rng >> 3)) & 1u) << 16);
            }
        }
        const uint8_t nout = (uint8_t)(rng & 1u);

        // Envelope.
        if (!eholding) {
            if ((uint32_t)(++ecount) >= eperiod) {
                ecount = 0;
                estep--;
                if (estep < 0) {
                    if (ehold) {
                        if (ealt) eattack ^= ENV_STEP_MASK;
                        eholding = 1;
                        estep = 0;
                    } else {
                        if (ealt && (estep & (ENV_STEP_MASK + 1)))
                            eattack ^= ENV_STEP_MASK;
                        estep &= ENV_STEP_MASK;
                    }
                }
            }
        }
        const uint8_t evol = (uint8_t)((estep ^ eattack) & ENV_STEP_MASK);

        // "(ToneOn | ToneDisable) & (NoiseOn | NoiseDisable)" -- MAME's own
        // comment. Register 7's bits are INVERTED enables, so a set bit
        // forces its term to 1. Both set means the output sits at 1, NOT 0,
        // and the channel can still be played by modulating volume alone.
        const uint8_t e0 = (uint8_t)((t0 | tdis0) & (nout | ndis0));
        const uint8_t e1 = (uint8_t)((t1 | tdis1) & (nout | ndis1));
        const uint8_t e2 = (uint8_t)((t2 | tdis2) & (nout | ndis2));

        acc0 += tab0[e0 ? (env0 ? evol : fix0) : 0];
        acc1 += tab1[e1 ? (env1 ? evol : fix1) : 0];
        acc2 += tab2[e2 ? (env2 ? evol : fix2) : 0];
    }

    // --- state back ---------------------------------------------------------
    a->tone_count[0] = c0; a->tone_count[1] = c1; a->tone_count[2] = c2;
    a->tone_out[0] = t0;   a->tone_out[1] = t1;   a->tone_out[2] = t2;
    a->noise_count = ncount;
    a->noise_prescale = npre;
    a->rng = rng;
    a->env_count = ecount;
    a->env_step = estep;
    a->env_attack = eattack;
    a->env_holding = eholding;
    a->env_volume = (uint8_t)((estep ^ eattack) & ENV_STEP_MASK);

    *out0 = acc0; *out1 = acc1; *out2 = acc2;
}


// ---------------------------------------------------------------------------
// The discrete network
// ---------------------------------------------------------------------------
//
// DISCRETE_SOUND_START( btime_sound_discrete ), btime.cpp:2216:
//
//     ADDER3(NODE_20, 1A, 1B, 1C); ADDER3(NODE_21, NODE_20, 2B, 2C)
//     MULTIPLY(NODE_22, NODE_21, 0.2)
//     OP_AMP_FILTER(NODE_30, 2A, BAND_PASS_1M, &btime_opamp_desc)
//     MIXER2(NODE_40, NODE_22, NODE_30, &btime_sound_mixer_desc)
//     CRFILTER(NODE_41, NODE_40, RES_K(10), CAP_U(10))
//     CRFILTER(NODE_43, NODE_41, 3.0, CAP_U(100))     // speaker model
//
// THE BAND-PASS on channel 2A is the one genuinely characteristic piece of
// analog here, and it is worth knowing what it does before judging the
// sound. Component values, all measured on a real PCB by "Anoid" per the
// driver's comment, with R51 documented there as a deliberate hack (the
// real 1k gives a gain of 23.5, so MAME uses 5k "with the modification,
// sound levels are in line with observations"):
//
//     r1 = R51 = 5k, r3 = R50 = 10k, rF = R49 = 47k, c1 = c2 = 0.068uF
//
// dst_op_amp_filt's DISC_OP_AMP_FILTER_IS_BAND_PASS_1M reset code
// (disc_flt.hxx:473) turns those into a second-order section:
//
//     rTotal = r1 || r3                                    = 3333.33
//     fc     = 1 / (2*pi*sqrt(rTotal*rF*c1*c2))            = 186.99 Hz
//     d      = (c1+c2) / sqrt(rF/rTotal*c1*c2)             = 0.532624
//     gain   = -rF/rTotal * c2/(c1+c2)                     = -7.05
//
// so 2A is a BASS channel: a ~187 Hz peak with Q ~1.88 and 7x gain, rolling
// off hard above ~800 Hz. That is consistent with the driver's note that on
// two 1982 recordings "the filtered sound is way louder than the music".
//
// Coefficients below are calculate_filter2_coefficients() (disc_flt.hxx:221)
// evaluated at THIS port's 22050 Hz output rate, with MAME's pre-warping
// (wc = fs*2*tan(pi*fc/fs)) -- not at MAME's own internal rate, because the
// point is to realise the same analog filter at the rate we actually run.
#define BP_A1  (-1.9692312072f)
#define BP_A2  ( 0.9720299897f)
#define BP_B0  (-0.0985942864f) // gain already folded in
#define BP_B2  ( 0.0985942864f) // = -B0; B1 is 0 for a band-pass

// Op-amp mixer: DISC_MIXER_IS_OP_AMP with both inputs through 100k and
// rF = 10k, so each contributes rF/R = 0.1. Its 150pF across rF puts a pole
// at 1/(2*pi*10k*150pF) = 106 kHz, five times above our Nyquist and
// therefore deliberately not modelled.
#define MIX_GAIN 0.1f

// DC blocker: the 10k/10uF CR filter, fc = 1/(2*pi*10k*10uF) = 1.59 Hz.
// This is not optional polish -- the amplitude tables above carry a large
// DC offset by design, and this is what removes it.
#define DCB_RC   0.1f // 10k * 10uF, seconds
// a = RC / (RC + T), T = 1 / sample rate. Written out so the compiler folds
// it at build time rather than dividing at run time.
#define DCB_A (DCB_RC / (DCB_RC + 1.0f / (float)BTIME_AUDIO_SAMPLE_RATE))
// The second CR filter (3 ohm / 100uF, fc = 530 Hz) models the CABINET's
// 4-ohm speaker, and is DELIBERATELY NOT IMPLEMENTED here. It is an
// aggressive high-pass that would thin the sound considerably, and the
// Fruit Jam has its own speaker with its own response -- applying an
// arcade cabinet's speaker model on top of a different real speaker is
// modelling the wrong thing twice. Noted rather than silently dropped;
// if the sound is boomier than a real machine, this is the first thing to
// try adding back.

// Final output scale. NOT derived: MAME's DISCRETE_OUTPUT gain of
// 32767/5*35 is expressed in the netlist's volt-ish units, which do not
// survive the change of amplitude representation above, so this constant
// was chosen by measuring the actual peak of the mixed signal in
// tools/btime_host (--wav) and leaving headroom.
//
// AND THEN THE DEVICE CORRECTED THE MEASUREMENT. A 40-second host capture
// peaked at 64.5% of full scale with nothing clipped, which looked like
// comfortable headroom -- but the on-device heartbeat reported a peak of
// 37004 against a 32767 ceiling during play, i.e. real clipping on sound
// combinations the host capture never happened to hit. The gain is now set
// from the DEVICE's peak with margin, not the host's. A reminder that a
// capture is a sample of behaviour, not a bound on it.
#define OUTPUT_GAIN 300000.0f

// ---------------------------------------------------------------------------
// Sample ring
// ---------------------------------------------------------------------------
//
// Generation happens on Core 0, in slices inside the scanline loop, and the
// board's audio ISR only COPIES OUT of this ring. That split is deliberate
// and is the shape DEVNOTES.md #48 arrived at: synthesising inside the ISR
// (as ArcadeMachine_Pacman does, where it is cheap enough) would put a
// ~2200-AY-tick burst in an interrupt that must not starve the PicoDVI
// scanline queue, which has only ~555us of slack.
#define RING_SIZE   1024u  // power of two
#define RING_TARGET 512u   // samples kept queued ahead of the ISR

static int16_t  g_ring[RING_SIZE];
static volatile uint32_t g_ring_head; // write index (producer, Core 0)
static volatile uint32_t g_ring_tail; // read index  (consumer, ISR)
static uint32_t g_underruns, g_overruns;
static int32_t  g_peak;

// Rolling mean microseconds per frame spent generating audio.
static uint32_t g_cost_accum_us;   // this frame's slices so far
static uint32_t g_cost_sum_us;     // sum over the window
static uint32_t g_cost_frames;
static uint32_t g_cost_mean_us;

// 1 / (ticks << AY_LEVEL_SHIFT) for every tick count that can occur; see
// generate_one_sample(). Built in btime_audio_init().
static float g_inv_scale[17];

// Filter state, producer side only.
static float g_bp_x1, g_bp_x2, g_bp_y1, g_bp_y2;
static float g_dcb_prev_in, g_dcb_prev_out;

// AY ticks per output sample: 187500 / 22050 = 8.50340..., carried as a
// fraction so the pitch does not drift.
#define TICKS_NUM (BTIME_AY_STEP_RATE)
#define TICKS_DEN (BTIME_AUDIO_SAMPLE_RATE)
static uint32_t g_tick_accum;

BTIME_ARAMFUNC static int16_t generate_one_sample(void) {
    // Advance the chips by the right (fractional) number of ticks and
    // average, which is a box filter -- cheap, and enough anti-aliasing at
    // an 8.5:1 ratio.
    g_tick_accum += TICKS_NUM;
    uint32_t ticks = 0;
    while (g_tick_accum >= TICKS_DEN) { g_tick_accum -= TICKS_DEN; ticks++; }
    if (ticks == 0) ticks = 1;

    int32_t a0, a1, a2, b0, b1, b2;
    ay_run(&g_ay[0], ticks, &a0, &a1, &a2);
    ay_run(&g_ay[1], ticks, &b0, &b1, &b2);
    const int32_t acc_flat = a0 + a1 + a2 + b1 + b2; // 1A+1B+1C+2B+2C
    const int32_t acc_2a   = b0;                     // the filtered channel

    // THE RECIPROCAL IS TABULATED, NOT DIVIDED. This target builds with
    // -mfloat-abi=softfp and NO -mfpu, so every float operation here is
    // software-emulated and a divide costs on the order of 120 cycles. Two
    // of them per sample -- this one and the DC blocker's coefficient
    // below -- were about 0.5ms of a 16.66ms frame, spent recomputing
    // constants. `ticks` is only ever 8 or 9 (187500/22050 = 8.5034), so a
    // 17-entry table covers every value it can take.
    const float scale = g_inv_scale[ticks < 17u ? ticks : 16u];
    const float flat = (float)acc_flat * scale * 0.2f; // MULTIPLY(NODE_22, .., 0.2)
    const float x    = (float)acc_2a * scale;

    // Band-pass biquad, in MAME's own difference-equation form
    // (dst_op_amp_filt's step): y = -a1*y1 - a2*y2 + b0*x + b1*x1 + b2*x2.
    const float y = -BP_A1 * g_bp_y1 - BP_A2 * g_bp_y2
                  +  BP_B0 * x       + BP_B2 * g_bp_x2;
    g_bp_x2 = g_bp_x1; g_bp_x1 = x;
    g_bp_y2 = g_bp_y1; g_bp_y1 = y;

    const float mixed = MIX_GAIN * (flat + y);

    // One-pole CR high-pass: y = a*(y_prev + x - x_prev), a = RC/(RC+T).
    // `a` depends only on the RC product and the sample rate, so it is a
    // constant -- see the note on software floats above; this used to be
    // two divides per sample.
    const float dc = DCB_A * (g_dcb_prev_out + mixed - g_dcb_prev_in);
    g_dcb_prev_in = mixed;
    g_dcb_prev_out = dc;

    int32_t s = (int32_t)(dc * OUTPUT_GAIN);
    if (s > g_peak) g_peak = s;
    if (-s > g_peak) g_peak = -s;
    if (s >  32767) s =  32767;
    if (s < -32768) s = -32768;
    return (int16_t)s;
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void btime_audio_address_w(uint8_t chip, uint8_t value) {
    if (chip > 1) return;
    // The AY latches the low 4 bits as the register index. The upper bits
    // are a chip-select field on a real 8910; both chips here have their own
    // address window in the sound CPU's map, so selection is by address.
    g_ay[chip].addr_latch = (uint8_t)(value & 0x0F);
}

void btime_audio_data_w(uint8_t chip, uint8_t value) {
    if (chip > 1) return;
    ay_write(&g_ay[chip], g_ay[chip].addr_latch, value);
    g_reg_writes++;
}

// __not_in_flash_func: the same deliberate, isolated exception
// ArcadeMachine_Invaders's invaders_audio.cpp documents in full (see that
// file and DEVNOTES.md problem #3/#7) -- this runs in the board's audio ISR
// and must never execute from flash, because an XIP cache miss inside the
// audio ISR is long enough to starve the PicoDVI scanline queue and shows
// up on screen as coloured glitch lines.
//
// All it does is copy: the synthesis happens on Core 0 (see
// btime_audio_run_slice()).
static void __not_in_flash_func(fill_audio)(int32_t *out, int count) {
    uint32_t tail = g_ring_tail;
    const uint32_t head = g_ring_head;

    for (int i = 0; i < count; i++) {
        int16_t s = 0;
        if (tail != head) {
            s = g_ring[tail & (RING_SIZE - 1u)];
            tail++;
        } else {
            g_underruns++; // ran dry: Core 0 is not keeping up
        }
        // Same packing every machine here uses: one int32 per stereo frame,
        // both channels carrying the same mono mix.
        out[i] = ((int32_t)s << 16) | (uint16_t)s;
    }
    g_ring_tail = tail;
}

void btime_audio_init(void) {
    ay_reset(&g_ay[0], false); // ay1: 5k on all three channels
    ay_reset(&g_ay[1], true);  // ay2: 1k on channel A, 5k on B and C
    g_reg_writes = 0;
    g_ring_head = g_ring_tail = 0;
    g_underruns = g_overruns = 0;
    g_peak = 0;
    g_tick_accum = 0;
    for (uint32_t i = 1; i < 17; i++)
        g_inv_scale[i] = 1.0f / ((float)i * (float)(1 << AY_LEVEL_SHIFT));
    g_inv_scale[0] = g_inv_scale[1]; // ticks is clamped to >= 1
    g_bp_x1 = g_bp_x2 = g_bp_y1 = g_bp_y2 = 0.0f;
    g_dcb_prev_in = g_dcb_prev_out = 0.0f;
    memset(g_ring, 0, sizeof(g_ring));

    // WARM THE FILTERS UP before anything can be heard. Both the DC blocker
    // and the band-pass start with zeroed state while their input sits at
    // the amplitude tables' idle DC level (see those tables' comment), so
    // without this the first fraction of a second is a decaying thump as
    // they settle -- measured at RMS 678 for half a second in a host
    // capture, i.e. an audible click on every boot. Running the generator
    // dry here settles them against the true idle DC, which is what the
    // registers hold at this point anyway.
    //
    // 4410 samples is 200ms at 22050Hz, several time constants of the
    // 187Hz band-pass; the 1.59Hz DC blocker needs far longer to settle
    // fully, but its remaining drift is inaudible and it is not the part
    // that thumps.
    for (int i = 0; i < 4410; i++) (void)generate_one_sample();
    g_peak = 0; // the warm-up is not a real peak

    hal_audio_set_fill_callback(&fill_audio);
}

// Tops the ring back up to RING_TARGET, a few samples at a time.
//
// Paced off the RING'S OWN FILL LEVEL rather than off a samples-per-frame
// constant, and that is the point: the ISR consumes at exactly 22050 Hz of
// REAL time while frames arrive at whatever rate the DVI pump allows, so a
// producer clocked off the emulated frame would drift against the consumer
// forever. DEVNOTES.md's cycle-vs-real-time audio-clock lesson is the same
// mistake in a different costume. Self-levelling here needs no rate
// constant at all.
BTIME_ARAMFUNC void btime_audio_run_slice(uint32_t slice, uint32_t slice_count) {
    // Cap per slice so no single slice becomes a long uninterrupted burst
    // between two scanline submissions (DEVNOTES.md #18/#20/#34/#36/#48).
    // 8 x 240 slices = 1920 samples of catch-up per frame, against the ~368
    // actually consumed, so recovery from a stall is quick without any
    // slice being expensive.
#if BTIME_AUDIO_PROFILING
    const uint32_t t0 = micros();
#endif

    uint32_t budget = 8;

    while (budget--) {
        const uint32_t head = g_ring_head;
        const uint32_t queued = head - g_ring_tail;
        if (queued >= RING_TARGET) break;
        g_ring[head & (RING_SIZE - 1u)] = generate_one_sample();
        g_ring_head = head + 1u;
        if (queued + 1u >= RING_SIZE) { g_overruns++; break; }
    }

#if BTIME_AUDIO_PROFILING
    g_cost_accum_us += micros() - t0;
#endif

    // The last slice of the frame closes the window. Averaged over 60
    // frames so a single expensive frame does not dominate the number the
    // heartbeat prints.
    if (slice_count && slice + 1u >= slice_count) {
        g_cost_sum_us += g_cost_accum_us;
        g_cost_accum_us = 0;
        if (++g_cost_frames >= 60u) {
            g_cost_mean_us = g_cost_sum_us / g_cost_frames;
            g_cost_sum_us = 0;
            g_cost_frames = 0;
        }
    }
}

uint32_t btime_audio_debug_cost_us(void) { return g_cost_mean_us; }

uint32_t btime_audio_debug_take_reg_writes(void) {
    uint32_t n = g_reg_writes;
    g_reg_writes = 0;
    return n;
}

void btime_audio_debug_take_stats(uint32_t *out_underruns, uint32_t *out_overruns,
                                  uint32_t *out_queued, int32_t *out_peak) {
    if (out_underruns) *out_underruns = g_underruns;
    if (out_overruns)  *out_overruns  = g_overruns;
    if (out_queued)    *out_queued    = g_ring_head - g_ring_tail;
    if (out_peak)      *out_peak      = g_peak;
    g_underruns = 0;
    g_overruns = 0;
    g_peak = 0;
}
