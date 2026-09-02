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
#include <Arduino.h> // micros(), for the cost measurement below -- the same
                     // instrumentation dkong_machine.cpp carries, because
                     // DEVNOTES.md #48 was caused by an audio cost nobody
                     // had measured.

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
static inline void ay_tick(ay_t *a, int32_t out[3]) {
    for (int c = 0; c < 3; c++) {
        const int32_t period = a->tone_period[c] ? a->tone_period[c] : 1; // std::max<int>(1, ...)
        a->tone_count[c]++;
        while (a->tone_count[c] >= period) {
            a->tone_out[c] ^= 1;
            a->tone_count[c] -= period;
        }
    }

    // Noise: the period is used RAW, with no max(1,...) clamp -- unlike the
    // tone period. With a period of 0 the comparison is true every tick, so
    // the prescaler toggles every tick; that is MAME's behaviour and the
    // real chip's.
    if (++a->noise_count >= (int32_t)(a->regs[AY_NOISEPER] & 0x1F)) {
        a->noise_count = 0;
        a->noise_prescale ^= 1;
        if (!a->noise_prescale) {
            // 17-bit LFSR, feedback bit0 XOR bit3, output bit0. ay8910.h's
            // comment notes this was verified on real AY-3-8910 and YM2149
            // parts.
            a->rng = (a->rng >> 1) | (((a->rng ^ (a->rng >> 3)) & 1u) << 16);
        }
    }
    const uint8_t noise_out = (uint8_t)(a->rng & 1u);

    // "(ToneOn | ToneDisable) & (NoiseOn | NoiseDisable)" -- MAME's own
    // comment. Register 7's bits are INVERTED enables, so a set bit forces
    // that term to 1. Both set means the output is stuck at 1, NOT 0, and
    // the channel can then still be played by modulating its volume alone.
    // Getting that backwards silences exactly the sounds that use it.
    uint8_t vol_enabled[3];
    for (int c = 0; c < 3; c++) {
        const uint8_t tone_dis  = (uint8_t)((a->regs[AY_ENABLE] >> c) & 1u);
        const uint8_t noise_dis = (uint8_t)((a->regs[AY_ENABLE] >> (3 + c)) & 1u);
        vol_enabled[c] = (uint8_t)((a->tone_out[c] | tone_dis) & (noise_out | noise_dis));
    }

    // Envelope.
    if (!a->env_holding) {
        const uint32_t period = a->env_period * ENV_STEP_MUL;
        if ((uint32_t)(++a->env_count) >= period) {
            a->env_count = 0;
            a->env_step--;
            if (a->env_step < 0) {
                if (a->env_hold) {
                    if (a->env_alternate) a->env_attack ^= ENV_STEP_MASK;
                    a->env_holding = 1;
                    a->env_step = 0;
                } else {
                    if (a->env_alternate && (a->env_step & (ENV_STEP_MASK + 1)))
                        a->env_attack ^= ENV_STEP_MASK;
                    a->env_step &= ENV_STEP_MASK;
                }
            }
        }
    }
    a->env_volume = (uint8_t)(a->env_step ^ a->env_attack);

    for (int c = 0; c < 3; c++) {
        const uint8_t volreg = a->regs[AY_AVOL + c];
        if (volreg & 0x10) { // envelope select (bit 4 of the volume register)
            out[c] = a->env_tab[c][vol_enabled[c] ? (a->env_volume & 0x0F) : 0];
        } else {
            out[c] = a->vol_tab[c][vol_enabled[c] ? (volreg & 0x0F) : 0];
        }
    }
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
// tools/btime_host (--wav) and leaving comfortable headroom: over a 40s
// capture through the level-start music and gameplay this peaks at about
// 65% of full scale with no clipped samples. Exactly the kind of value
// CLAUDE.md says can only really be judged on hardware -- adjust here if
// it is too quiet, too loud, or clips.
#define OUTPUT_GAIN 500000.0f

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

// Filter state, producer side only.
static float g_bp_x1, g_bp_x2, g_bp_y1, g_bp_y2;
static float g_dcb_prev_in, g_dcb_prev_out;

// AY ticks per output sample: 187500 / 22050 = 8.50340..., carried as a
// fraction so the pitch does not drift.
#define TICKS_NUM (BTIME_AY_STEP_RATE)
#define TICKS_DEN (BTIME_AUDIO_SAMPLE_RATE)
static uint32_t g_tick_accum;

static int16_t generate_one_sample(void) {
    // Advance the chips by the right (fractional) number of ticks and
    // average, which is a box filter -- cheap, and enough anti-aliasing at
    // an 8.5:1 ratio.
    g_tick_accum += TICKS_NUM;
    uint32_t ticks = 0;
    while (g_tick_accum >= TICKS_DEN) { g_tick_accum -= TICKS_DEN; ticks++; }
    if (ticks == 0) ticks = 1;

    int32_t acc_flat = 0; // 1A + 1B + 1C + 2B + 2C
    int32_t acc_2a   = 0;
    for (uint32_t t = 0; t < ticks; t++) {
        int32_t o1[3], o2[3];
        ay_tick(&g_ay[0], o1);
        ay_tick(&g_ay[1], o2);
        acc_flat += o1[0] + o1[1] + o1[2] + o2[1] + o2[2];
        acc_2a   += o2[0];
    }

    const float scale = 1.0f / ((float)ticks * (float)(1 << AY_LEVEL_SHIFT));
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
    const float a = DCB_RC / (DCB_RC + 1.0f / (float)BTIME_AUDIO_SAMPLE_RATE);
    const float dc = a * (g_dcb_prev_out + mixed - g_dcb_prev_in);
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
void btime_audio_run_slice(uint32_t slice, uint32_t slice_count) {
    // Cap per slice so no single slice becomes a long uninterrupted burst
    // between two scanline submissions (DEVNOTES.md #18/#20/#34/#36/#48).
    // 8 x 240 slices = 1920 samples of catch-up per frame, against the ~368
    // actually consumed, so recovery from a stall is quick without any
    // slice being expensive.
    const uint32_t t0 = micros();

    uint32_t budget = 8;

    while (budget--) {
        const uint32_t head = g_ring_head;
        const uint32_t queued = head - g_ring_tail;
        if (queued >= RING_TARGET) break;
        g_ring[head & (RING_SIZE - 1u)] = generate_one_sample();
        g_ring_head = head + 1u;
        if (queued + 1u >= RING_SIZE) { g_overruns++; break; }
    }

    g_cost_accum_us += micros() - t0;

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
