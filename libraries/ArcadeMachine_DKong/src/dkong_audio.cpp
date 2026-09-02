// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Donkey Kong sound implementation -- see dkong_audio.h for what is
// emulated, what is approximated, and why.
#include <string.h>
#include <math.h>
#include "dkong_audio.h"
#include "arcade_hal_audio.h"
#include "mcs48.h"
#include <Arduino.h> // micros() for the cost instrument below
#include "pico.h" // __not_in_flash_func -- a deliberate, documented
                  // exception to board-agnosticism (DEVNOTES.md problem #7)

// The per-sample synthesis and the 8035 callbacks around it run ~364 times
// per frame inside an already-tight budget, so they belong in SRAM for the
// same reason the CPU cores do. Raw section attribute rather than
// __not_in_flash_func() so the host harness still compiles this file.
#if defined(ARDUINO_ARCH_RP2040) || defined(PICO_ON_DEVICE)
#define DKA_RAMFUNC __attribute__((section(".time_critical.dkonga")))
#else
#define DKA_RAMFUNC
#endif

uint8_t dkong_sound_rom[DKONG_SOUND_ROM_SIZE];
uint8_t dkong_tune_rom[DKONG_TUNE_ROM_SIZE];

// --- board state ---------------------------------------------------------

static mcs48   g_cpu;
static uint8_t g_cmd_latch;   // MAME's "ls175.3d", written at 0x7C00
static uint8_t g_sig_latch;   // MAME's m_dev_6h, written at 0x7D00-0x7D07
static uint8_t g_p2_latch;    // MAME's m_dev_vp2 -- the 8035's own P2 output
static uint8_t g_dac;         // the 8035's port-1 latch: the DAC value

// Debug counters, declared here rather than beside the FIFO ones because
// snd_port_w() below is their first user.
static uint32_t g_p1_writes, g_p2_writes;
static uint8_t  g_dac_min = 0xFF, g_dac_max = 0;
static uint32_t g_dac_hist[256];

// latch8 read semantics from dkong2b_audio():
//   ls175_3d: set_maskout(0xf0), set_xorvalue(0x0f)
//   dev_vp2:  set_xorvalue(0x20), and bit 5 is read from dev_6h bit 3
//             rather than from the latch itself.
DKA_RAMFUNC static inline uint8_t cmd_latch_r(void) { return (uint8_t)((g_cmd_latch & 0x0F) ^ 0x0F); }
DKA_RAMFUNC static inline uint8_t p2_latch_r(void) {
    uint8_t v = (uint8_t)((g_p2_latch & ~0x20) | (((g_sig_latch >> 3) & 1) << 5));
    return (uint8_t)(v ^ 0x20);
}

// --- 8035 wiring ---------------------------------------------------------

DKA_RAMFUNC static uint8_t snd_program_r(mcs48 *cpu, uint16_t addr) {
    (void)cpu;
    return dkong_sound_rom[addr & (DKONG_SOUND_ROM_SIZE - 1)];
}

// dkong_tune_r(): P2 bit 6 selects between the command latch and a banked
// 256-byte window of the sample ROM, with P2 bits 0-2 as the page. The
// `offset` a real BUS read supplies is the low byte of the address the 8035
// puts out, which for this board is whatever it last drove -- MAME passes
// the I/O map offset. INS A,BUS has no address operand, so the page offset
// comes from the 8035's own R0/R1 pointer usage; MAME's io map makes the
// whole 0x00-0xFF range decode to this handler and passes the low address
// byte. Here the 8035's last P1 write is NOT the address; the real board
// wires the ROM's low address lines to the 8035's BUS latch, which for the
// INS A,BUS path is the accumulator-driven address. Using A is what makes
// the tunes play; see DEVNOTES.md #44.
// dkong_sound_io_map(): map(0x00, 0xff).rw(dkong_tune_r, dkong_voice_w).
// That is the AS_IO space, which on an MCS-48 is reached by MOVX A,@Rn with
// the register as the address -- this is how the sample ROM is actually
// read. dkong_tune_r() is ALSO wired as bus_in_cb for INS A,BUS, where the
// offset is 0; both go through tune_read() below with a different address.
DKA_RAMFUNC static uint8_t tune_read(uint8_t offset) {
    uint8_t page = (uint8_t)(p2_latch_r() & 0x47);
    if (page & 0x40) {
        // Command latch, with the (unimplemented) voice-status nibble in
        // the high bits -- dkong_voice_status_r() returns 0 in MAME too.
        return (uint8_t)(cmd_latch_r() & 0x0F);
    }
    return dkong_tune_rom[(uint32_t)((page & 7) * 256 + offset) & (DKONG_TUNE_ROM_SIZE - 1)];
}

DKA_RAMFUNC static uint8_t snd_ext_r(mcs48 *cpu, uint8_t addr) {
    (void)cpu;
    return tune_read(addr);
}

DKA_RAMFUNC static void snd_ext_w(mcs48 *cpu, uint8_t addr, uint8_t data) {
    (void)cpu; (void)addr; (void)data; // dkong_voice_w(): "not actually used"
}

DKA_RAMFUNC static uint8_t snd_bus_r(mcs48 *cpu) {
    (void)cpu;
    // bus_in_cb passes no address, so MAME's handler sees offset 0.
    return tune_read(0);
}

DKA_RAMFUNC static void snd_bus_w(mcs48 *cpu, uint8_t data) {
    (void)cpu; (void)data; // dkong_voice_w(): "not actually used"
}

DKA_RAMFUNC static uint8_t snd_port_r(mcs48 *cpu, uint8_t port) {
    (void)cpu;
    return (port == 2) ? p2_latch_r() : 0xFF;
}

DKA_RAMFUNC static void snd_port_w(mcs48 *cpu, uint8_t port, uint8_t data) {
    (void)cpu;
    if (port == 1) {
        g_dac = data;                 // dkong_p1_w(): "only write to dac"
        g_p1_writes++;
        if (data < g_dac_min) g_dac_min = data;
        if (data > g_dac_max) g_dac_max = data;
        g_dac_hist[data]++;
    } else if (port == 2) { g_p2_latch = data; g_p2_writes++; }
}

// T0 and T1 are the INVERTED bits 5 and 4 of the signal latch
// (bit5_q_r / bit4_q_r).
DKA_RAMFUNC static uint8_t snd_test_r(mcs48 *cpu, uint8_t line) {
    (void)cpu;
    uint8_t bit = (line == 0) ? 5 : 4;
    return (uint8_t)(((g_sig_latch >> bit) & 1) ^ 1);
}

// --- discrete channel approximations -------------------------------------
//
// Each of the three stands in for one DISCRETE_TASK in dkong2b_discrete.
// None is a circuit simulation; each keeps the shape that makes the sound
// recognisable and drops the analog detail.

// "Stomp" (DS_OUT_SOUND0, triggered by signal bit 2) -- Kong's climb.
//
// The clock rate is the whole character of this sound and is DERIVED, not
// chosen: MAME clocks the LFSR at CLOCK_2VF, and dkong.h defines that as
// MASTER_CLOCK/5/4/16/12/2/2 = 61.44MHz -> 4000 Hz. An LS161 then divides
// it and NODE_13 is (counter > 3), so the audible content sits around
// 4000/8 = 500 Hz before the RC integrator low-passes it further.
//
// Generating white noise at the AUDIO rate instead of at 4kHz is what makes
// this sound like a woodblock or a whip rather than a deep boom -- the
// difference between 22050 Hz and 4000 Hz of noise bandwidth is the entire
// difference between the two sounds. Reported by ear before it was found by
// derivation; see DEVNOTES.md #46.
#define STOMP_LFSR_HZ 4000.0f
static float    g_stomp_env, g_stomp_clk, g_stomp_lp;
static uint8_t  g_stomp_count;   // the LS161, 0..7
static uint8_t  g_stomp_prev;    // for rising-edge detection on the LFSR
static uint32_t g_noise_lfsr = 0x1234;

// "Jump" (DS_OUT_SOUND1, signal bit 1) and "Walk" (DS_OUT_SOUND2, signal
// bit 0). MAME: a CD4049 inverter oscillator sweeping the control voltage
// of a 555 astable, plus an RC-decayed trigger. Kept: a square oscillator
// whose frequency sweeps over the life of an envelope. Dropped: the 555's
// duty cycle and the exact CV curve.
//
// The BASE frequencies are derived from the real components rather than
// picked by ear. Both 555s are DISCRETE_555_ASTABLE_CV(RES_K(47),
// RES_K(27), C) and f = 1.44 / ((R1 + 2*R2) * C):
//     jump  C = 47nF -> 303 Hz
//     walk  C = 33nF -> 432 Hz
// The CV sweep then moves them; only the sweep shape is guesswork.
// Derived base frequencies (above) put the 555s in the right region; the
// numbers actually used are MEASURED off the reference recordings in
// dk_sounds/, because the CV sweep -- which the derivation cannot supply --
// dominates what you hear. See DEVNOTES.md #46.
#define JUMP_CENTRE_HZ 362.0f   // jump.wav: warbles 233..467Hz, mean 372Hz
#define JUMP_DEPTH_HZ  117.0f
#define JUMP_DEPTH_FLOOR 0.55f  // the warble narrows to ~55% by the tail
#define JUMP_LEVEL      0.12f   // 0.32 -> 0.18 -> 0.12, each step by ear
#define JUMP_DECAY      0.99977f // ~0.52s audible span, per the reference
#define JUMP_LFO_HZ     10.0f   // ~0.10s period, measured peak-to-peak
// walk.wav's measured contour is a dip then a steep climb, per step. The
// numbers below are the raw measurement (500/267/700) with the DIP raised
// to 340. The reason is worth recording, because the obvious metric misled:
// the mean pitch of the raw contour already matched the reference exactly
// (477Hz both), yet it sounded low -- because the chirp's early, loudest
// portion sat at 300-370Hz where the real one sits at 430-530, and that
// opening is what the ear judges. Raising the whole contour to 560/400/760
// then overshot to a 589Hz mean. Lifting only the dip keeps the mean at
// ~475 while holding the loud opening high.
#define WALK_F_START   500.0f
#define WALK_F_DIP     340.0f
#define WALK_F_END     700.0f
#define WALK_DIP_AT      0.45f  // fraction of the chirp at which it bottoms out
#define WALK_CHIRP_S     0.15f
// Gate shaping and level. The level came down from 0.26 because the old
// one-shot was audibly dominating the mix; the real thing sits under the
// music rather than over it.
#define WALK_LEVEL     0.045f // sits well under the music
#define WALK_ATTACK    0.0065f  // ~7ms
#define WALK_RELEASE   0.0011f  // ~40ms
static float g_jump_env, g_jump_phase, g_jump_lfo, g_jump_attack;
static float g_walk_env, g_walk_phase, g_walk_t;

// DAC decay ("Signal decay circuit Q7, R20, C32") and the Sallen-Key
// low-pass that follows it. MAME's own comment gives the filter directly:
// f = 1916 Hz, Q = 0.74.
static float g_dac_decay;
static float g_dac_dc;   // running DC estimate for the AC-coupling high-pass
static float g_sk_z1, g_sk_z2, g_sk_a0, g_sk_a1, g_sk_a2, g_sk_b1, g_sk_b2;

// Sine table for the two LFOs. sinf()/cosf() are library calls on
// Cortex-M33 and were being made once per audio sample; a 256-entry table
// costs 1KB and turns each into an index and a load. Deliberately NOT
// const: a const table lives in flash, and reading flash once per sample in
// this loop is the very cost this change exists to remove.
#define SINTAB_SIZE 256
static float g_sintab[SINTAB_SIZE];
static inline float lfo_sin(float phase01) {
    return g_sintab[((unsigned)(phase01 * SINTAB_SIZE)) & (SINTAB_SIZE - 1)];
}
static inline float lfo_cos(float phase01) {
    return g_sintab[((unsigned)(phase01 * SINTAB_SIZE) + SINTAB_SIZE / 4) & (SINTAB_SIZE - 1)];
}

static void sallen_key_init(float f0, float q, float fs) {
    float w = 2.0f * 3.14159265f * f0 / fs;
    float cw = cosf(w), sw = sinf(w);
    float alpha = sw / (2.0f * q);
    float b0 = (1.0f - cw) * 0.5f, b1 = 1.0f - cw, b2 = (1.0f - cw) * 0.5f;
    float a0 = 1.0f + alpha, a1 = -2.0f * cw, a2 = 1.0f - alpha;
    g_sk_a0 = b0 / a0; g_sk_a1 = b1 / a0; g_sk_a2 = b2 / a0;
    g_sk_b1 = a1 / a0; g_sk_b2 = a2 / a0;
}

// --- sample FIFO ---------------------------------------------------------
//
// The producer (dkong_audio_run_frame, on the main core) and the consumer
// (the board's audio ISR) are decoupled by this ring. Depth is generous on
// purpose: a frame produces ~364 samples and the ISR drains in bursts of
// whatever the board's buffer size is.
#define FIFO_SIZE 2048
#define FIFO_MASK (FIFO_SIZE - 1)
static int16_t          g_fifo[FIFO_SIZE];
static volatile uint16_t g_fifo_head, g_fifo_tail;
static uint32_t g_underruns, g_overruns, g_peak_depth, g_sound_cycles;
// How many times each discrete channel was triggered. Without these,
// "I cannot hear the jump" is ambiguous between a synthesis that is wrong
// and a trigger that never fired -- which is exactly the confusion that
// prompted them.
static uint32_t g_trig_walk, g_trig_jump, g_trig_stomp;
// Everything the MAIN CPU asks the sound hardware to do. If sounds are
// missing, the first question is whether they were ever requested -- and
// that is a different file from the one that would be blamed otherwise.
static uint32_t g_cmd_writes, g_irq_asserts, g_sig_bit_writes[8];
// How many SAMPLES each signal bit spent HIGH. A bit that is held for long
// stretches is a gate, not a one-shot trigger, and wants completely
// different synthesis.
static uint32_t g_sig_bit_high[8], g_sig_samples;
// Channel solo/mute for diagnostics. The discrete channels sit UNDER the
// music by design, which makes them hard to measure in the mix -- and
// 'I cannot see it in the spectrum' is not the same as 'it is not there'.
// Bit 0 = DAC, 1 = stomp, 2 = jump, 3 = walk; all enabled by default.
static uint8_t g_chan_mask = 0x0F;
void dkong_audio_debug_set_channels(uint8_t mask) { g_chan_mask = mask; }
static uint8_t  g_cmd_seen[16];   // which command nibbles the game sent
// Weak hook so a harness can print WHEN each trigger fired; a no-op on
// device. "Which timestamp is the jump?" is not answerable from a count.
__attribute__((weak)) void dkong_audio_debug_trigger_event(const char *name) { (void)name; }

static inline uint16_t fifo_depth(void) {
    return (uint16_t)((g_fifo_head - g_fifo_tail) & FIFO_MASK);
}

// --- debug trace ---------------------------------------------------------
//
// Dumps the sound CPU's instruction stream. Added after the DAC path was
// debugged from the outside in for far too long: "is the 8035 executing the
// code I think it is" is a question about the CPU, and no amount of staring
// at the audio output answers it. See DEVNOTES.md #45.
static long g_trace_left;
// Supplied by the harness; a weak no-op keeps device builds free of it.
__attribute__((weak)) void dkong_audio_debug_trace_line(
        uint16_t pc, uint8_t op, uint8_t a, uint8_t dac, uint8_t p2,
        uint8_t psw, uint8_t timer) {
    (void)pc; (void)op; (void)a; (void)dac; (void)p2; (void)psw; (void)timer;
}
void dkong_audio_debug_trace(long instructions) { g_trace_left = instructions; }

// --- generation ----------------------------------------------------------

// 8035 machine cycles per audio sample: 6MHz/15 = 400,000 cycles/sec
// against DKONG_AUDIO_SAMPLE_RATE.
#define SOUND_CYCLES_PER_SAMPLE (400000.0f / (float)DKONG_AUDIO_SAMPLE_RATE)

DKA_RAMFUNC static int16_t generate_one_sample(void) {
    const float fs = (float)DKONG_AUDIO_SAMPLE_RATE;

    // --- DAC path (emulated) ---
    // NODE_71 in MAME: the DAC value scaled to volts, multiplied by a decay
    // envelope that is held high while the discharge line is asserted.
    // P2 bit 7 is DS_DISCHARGE_INV; when it is LOW the signal is held.
    // THE DAC DECAY CIRCUIT (Q7/R20/C32) IS DELIBERATELY NOT MODELLED.
    //
    // MAME's NODE_71 is
    //     gain = NODE_70 + !DS_DISCHARGE_INV
    // with NODE_70 a DISCRETE_RCDISC driven by the same signal, and
    // DS_DISCHARGE_INV coming from the 8035's port-2 bit 7. Implementing
    // that literally, with the decay starting high, produced audio for
    // exactly 0.84 seconds and then permanent silence -- because this
    // game's 8035 writes port 2 only twice in 900 frames and leaves it at
    // 0xFF, so bit 7 is high essentially always.
    //
    // A gate that mutes the DAC in the state the sound program actually
    // sits in cannot be right, and rather than guess at the polarity of a
    // circuit whose MAME model has not been read end to end, the gate is
    // left out. The cost is a small one: the real circuit's job is to damp
    // the DAC between notes, so omitting it means slightly more residual
    // noise between sounds, not a wrong pitch or a missing effect.
    //
    // What would settle it: read DISCRETE_RCDISC's implementation in MAME's
    // discrete engine (src/emu/sound/disc_rc.* ) and confirm what NODE_70
    // reads while its enable is high. See DEVNOTES.md #45.
    (void)g_dac_decay;
    // THE DAC IS UNIPOLAR. MAME's NODE_71 scales it as DS_DAC * (SUP_V/256):
    // a straight ramp from 0 volts at 0x00 to full scale at 0xFF, NOT a
    // signed value centred on 0x80. Treating it as centred makes the IDLE
    // state -- which this program spends most of its time in, writing 0x00 --
    // come out as a full-scale negative DC offset. The symptom is audio whose
    // RMS exactly equals its peak: a constant, which is neither silence nor
    // sound and looks like a stuck output. See DEVNOTES.md #45.
    //
    // The speaker is AC-coupled on the real board, so the DC has to go
    // somewhere: a one-pole high-pass at roughly 20Hz does that job here.
    float dac_uni = (float)g_dac / 255.0f;
    g_dac_dc += (dac_uni - g_dac_dc) * 0.0057f; // ~20Hz at 22050Hz
    float dac = dac_uni - g_dac_dc;

    // Sallen-Key low-pass, f=1916Hz Q=0.74 (MAME's own numbers).
    float out = g_sk_a0 * dac + g_sk_z1;
    g_sk_z1 = g_sk_a1 * dac - g_sk_b1 * out + g_sk_z2;
    g_sk_z2 = g_sk_a2 * dac - g_sk_b2 * out;
    float mix = (g_chan_mask & 1) ? out * 0.9f : 0.0f;

    // --- stomp (approximated) ---
    if (g_stomp_env > 0.0005f) {
        // Clock the LFSR at CLOCK_2VF (4kHz), not at the audio rate.
        g_stomp_clk += STOMP_LFSR_HZ / fs;
        while (g_stomp_clk >= 1.0f) {
            g_stomp_clk -= 1.0f;
            g_noise_lfsr = (g_noise_lfsr >> 1) ^
                           (uint32_t)(-(int32_t)(g_noise_lfsr & 1) & 0xB400u);
            uint8_t bit = (uint8_t)(g_noise_lfsr & 1);
            // LS161 (IC 3J): counts on the rising edge of the noise bit.
            if (bit && !g_stomp_prev) g_stomp_count = (uint8_t)((g_stomp_count + 1) & 7);
            g_stomp_prev = bit;
        }
        // NODE_13: (counter > 3) * SUP_V -- a square, not noise.
        float level = (g_stomp_count > 3) ? 1.0f : -1.0f;
        // RCINTEGRATE (C19): the low-pass that turns it into a boom.
        g_stomp_lp += (level - g_stomp_lp) * 0.06f;
        if (g_chan_mask & 2) mix += g_stomp_lp * g_stomp_env * 0.5f;
        // dk_sounds/thump.wav decays over ~0.6s; the previous constant
        // died in ~0.3s. Its dominant 125Hz matches what the 4kHz LFSR
        // through the LS161 already produces, which is why this one only
        // needed its envelope corrected and not its pitch.
        g_stomp_env *= 0.99977f;
    }

    // --- jump (approximated) ---
    // The 555's control voltage is swept by a CD4049 inverter oscillator,
    // and the audible result is a WARBLE, not a ramp. All four numbers
    // below are measured off dk_sounds/jump.wav:
    //   - centre 350Hz, warbling between 233 and 467
    //   - LFO period ~0.10s (10Hz)
    //   - audible span ~0.52s
    //   - and the warble NARROWS over the note: the early swings cover
    //     233..467, the late ones only 333..467. That is the CV sweep
    //     decaying along with the envelope, and leaving it out is what made
    //     an otherwise pitch-accurate jump sound mechanical -- the mean was
    //     already right (369Hz against the reference's 372).
    if (g_jump_env > 0.0005f) {
        g_jump_lfo += JUMP_LFO_HZ / fs;
        if (g_jump_lfo >= 1.0f) g_jump_lfo -= 1.0f;
        // Depth shrinks with the envelope, never below ~55% of nominal.
        float depth = JUMP_DEPTH_HZ * (JUMP_DEPTH_FLOOR +
                                       (1.0f - JUMP_DEPTH_FLOOR) * g_jump_env);
        float f = JUMP_CENTRE_HZ + depth * lfo_sin(g_jump_lfo);
        g_jump_phase += f / fs;
        if (g_jump_phase >= 1.0f) g_jump_phase -= 1.0f;
        if (g_jump_attack < 1.0f) g_jump_attack += 1.0f / (0.06f * fs);
        float a = g_jump_attack < 1.0f ? g_jump_attack : 1.0f;
        if (g_chan_mask & 4) mix += (g_jump_phase < 0.5f ? JUMP_LEVEL : -JUMP_LEVEL) * g_jump_env * a;
        g_jump_env *= JUMP_DECAY;
    }

    // --- walk (approximated) ---
    // Gated (the game HOLDS signal bit 0 for ~49ms per step, measured), but
    // the pitch follows a DETERMINISTIC CHIRP restarted on each step rather
    // than a free-running LFO.
    //
    // That distinction is the whole character of this sound. Measured off
    // dk_sounds/walk.wav, one step sweeps:
    //     ~500Hz -> dips to ~267Hz at about 45% through -> climbs to ~700Hz
    // over roughly 0.15s. A free-running oscillator catches a different
    // phase of that curve on every step, so no two steps sound alike and
    // the result is mush -- reported as "too simple, not as charming as the
    // original" before it was measured. A restarted contour gives every
    // step the same recognisable shape. See DEVNOTES.md #47.
    {
        float gate = (g_sig_latch & 0x01) ? 1.0f : 0.0f;
        float k = (gate > g_walk_env) ? WALK_ATTACK : WALK_RELEASE;
        g_walk_env += (gate - g_walk_env) * k;
        if (g_walk_env > 0.0005f) {
            float x = g_walk_t / WALK_CHIRP_S;      // 0..1 through the chirp
            if (x > 1.0f) x = 1.0f;
            float f = (x < WALK_DIP_AT)
                ? WALK_F_START + (WALK_F_DIP - WALK_F_START) * (x / WALK_DIP_AT)
                : WALK_F_DIP   + (WALK_F_END - WALK_F_DIP)   * ((x - WALK_DIP_AT) / (1.0f - WALK_DIP_AT));
            g_walk_phase += f / fs;
            if (g_walk_phase >= 1.0f) g_walk_phase -= 1.0f;
            if (g_chan_mask & 8) mix += (g_walk_phase < 0.5f ? WALK_LEVEL : -WALK_LEVEL) * g_walk_env;
            g_walk_t += 1.0f / fs;
        }
    }

    if (mix >  1.0f) mix =  1.0f;
    if (mix < -1.0f) mix = -1.0f;
    return (int16_t)(mix * 12000.0f);
}

// How many samples this frame still wants. Computed once per frame and
// then drained in slices -- see dkong_audio_run_slice().
static int g_frame_want, g_frame_done;
// Accumulated microseconds spent generating sound, per frame. Measured
// where the work actually happens (inside the slices) rather than around
// a whole-frame call the interleaved path never makes -- an instrument
// that always reads zero is worse than none.
static uint32_t g_cost_us, g_cost_frames, g_cost_us_last;
uint32_t dkong_audio_debug_cost_us(void) { return g_cost_us_last; }

// Self-pacing rather than open-loop. Producing a fixed number of samples
// per frame would drift: this project's frames are paced by the real DVI
// rate (~60.0Hz), not by the board's nominal 60.606Hz, and DEVNOTES.md
// problem #34 is an entire entry about what that 0.8% mismatch does to an
// audio path over a few minutes. Topping the FIFO up to a target depth
// instead makes the consumer set the rate, so the two clocks cannot
// diverge. The bounds matter: without an upper limit a stalled consumer
// would let the 8035 sprint, and without a lower one it could stop.
DKA_RAMFUNC static void begin_frame(void) {
    const uint16_t target = 700;   // ~2 frames of slack
    const int max_samples = 900;
    int want = (int)target - (int)fifo_depth();
    if (want < 0)           want = 0;
    if (want > max_samples) want = max_samples;
    g_frame_want = want;
    g_frame_done = 0;
}

DKA_RAMFUNC static void produce(int n) {
    static float cycle_accum = 0.0f;
    uint32_t cycles_run = 0;

    for (int i = 0; i < n; i++) {
        // Run the sound CPU forward one audio sample's worth of time.
        cycle_accum += SOUND_CYCLES_PER_SAMPLE;
        while (cycle_accum >= 1.0f) {
            if (g_trace_left > 0) {
                g_trace_left--;
                dkong_audio_debug_trace_line(g_cpu.pc,
                    g_cpu.program_r(&g_cpu, g_cpu.pc), g_cpu.a, g_dac, g_p2_latch,
                    g_cpu.psw, g_cpu.timer);
            }
            uint8_t used = mcs48_step(&g_cpu);
            cycle_accum -= (float)used;
            cycles_run  += used;
        }

        uint16_t next = (uint16_t)((g_fifo_head + 1) & FIFO_MASK);
        if (next == g_fifo_tail) { g_overruns++; break; }
        for (int b = 0; b < 8; b++) if (g_sig_latch & (1u << b)) g_sig_bit_high[b]++;
        g_sig_samples++;
        g_fifo[g_fifo_head] = generate_one_sample();
        g_fifo_head = next;
    }

    g_sound_cycles += cycles_run;
    uint16_t d = fifo_depth();
    if (d > g_peak_depth) g_peak_depth = d;
}

// INTERLEAVED with the scanline pump, for exactly the reason DEVNOTES.md
// problems #18/#20/#34/#36 exist.
//
// This cost a flash cycle to learn AGAIN. The frame budget was never the
// issue: measured on hardware, video is ~9.4ms and sound ~2.9ms of a
// 16.66ms frame, which fits with room to spare. What broke was WHERE the
// 2.9ms sat. Running it all after the 240 scanline submissions leaves Core
// 0 not feeding the DVI queue for 2.9ms, and Core 1 can only coast on the
// 8-buffer queue (~555us, a hard libdvi ceiling) plus vertical blanking
// (~1.4ms) -- about 2ms. So the queue starved on EVERY frame, and frame
// pacing broke up (14.6ms/18.7ms alternating instead of a pinned 16.66ms).
//
// The CPU half of this frame was carefully interleaved from the start. The
// audio half was then bolted onto the end, which is the same mistake in a
// new place: it is not "is there budget", it is "is there ever a gap
// longer than ~2ms between two scanline submissions".
DKA_RAMFUNC void dkong_audio_run_slice(uint32_t slice, uint32_t nslices) {
    uint32_t t0 = micros();
    if (slice == 0) begin_frame();
    // Exact proportional target rather than repeated division, so the last
    // slice lands on the frame's full count however it divides.
    int target = (int)(((uint32_t)g_frame_want * (slice + 1)) / nslices);
    int n = target - g_frame_done;
    if (n > 0) { produce(n); g_frame_done += n; }

    g_cost_us += micros() - t0;
    if (slice + 1 == nslices) {
        if (++g_cost_frames >= 60) {
            g_cost_us_last = g_cost_us / g_cost_frames;
            g_cost_us = 0;
            g_cost_frames = 0;
        }
    }
}

// Whole-frame version, for the sequential (landscape/180) render path where
// there is no scanline pump to interleave with.
DKA_RAMFUNC void dkong_audio_run_frame(dkong_system *system) {
    (void)system;
    begin_frame();
    produce(g_frame_want);
    g_frame_done = g_frame_want;
}

// --- HAL fill callback ---------------------------------------------------

// Runs in the board's audio ISR, so it lives in SRAM: an XIP cache-miss
// stall here is long enough to starve the PicoDVI scanline queue and show
// up as coloured glitch lines (DEVNOTES.md problem #7). It does no
// synthesis -- all of that happens on the main core in run_frame -- so it
// stays short by construction.
static void __not_in_flash_func(dkong_audio_fill)(int32_t *out, int count) {
    for (int i = 0; i < count; i++) {
        int16_t s;
        if (g_fifo_tail == g_fifo_head) {
            s = 0;
            g_underruns++;
        } else {
            s = g_fifo[g_fifo_tail];
            g_fifo_tail = (uint16_t)((g_fifo_tail + 1) & FIFO_MASK);
        }
        // Packed as arcade_hal_audio.h specifies: both channels in one
        // int32_t, (sample << 16) | (uint16_t)sample. `count` is the number
        // of ENTRIES, not frames.
        out[i] = ((int32_t)s << 16) | (uint16_t)s;
    }
}

// --- public wiring -------------------------------------------------------

void dkong_audio_command_w(uint8_t data) {
    g_cmd_latch = data;
    g_cmd_writes++;
    g_cmd_seen[data & 0x0F] = 1;
}

void dkong_audio_signal_w(uint8_t offset, uint8_t data) {
    uint8_t bit = (uint8_t)(offset & 7);
    g_sig_bit_writes[bit]++;
    uint8_t prev = g_sig_latch;
    if (data & 1) g_sig_latch |= (uint8_t)(1u << bit);
    else          g_sig_latch &= (uint8_t)~(1u << bit);

    // Rising edges on the three discrete triggers start their envelopes.
    uint8_t rose = (uint8_t)(g_sig_latch & ~prev);
    // The walk is gated in generate_one_sample() from the live bit, so the
    // edge only counts the step for instrumentation.
    if (rose & 0x01) { g_walk_t = 0.0f; g_trig_walk++; dkong_audio_debug_trigger_event("walk"); }
    // jump.wav opens at ~333Hz and FALLS to its ~233Hz minimum before the
    // warble climbs. Starting the LFO at 0.55 puts it on that falling edge;
    // starting at 0 opened on the peak instead, which loses the attack.
    if (rose & 0x02) { g_jump_env = 1.0f; g_jump_phase = 0.0f; g_jump_lfo = 0.55f; g_jump_attack = 0.0f; g_trig_jump++; dkong_audio_debug_trigger_event("jump"); }
    if (rose & 0x04) { g_stomp_env = 1.0f; g_stomp_lp = 0.0f; g_trig_stomp++; dkong_audio_debug_trigger_event("stomp"); }
}

void dkong_audio_irq_w(uint8_t data) {
    if (data) g_irq_asserts++;
    mcs48_set_irq(&g_cpu, data != 0);
}

uint8_t dkong_audio_status_r(void) {
    // IN2 bit 6 = inverted P2 bit 4 (latch8 bit4_q_r on the raw latch).
    return (uint8_t)((((g_p2_latch >> 4) & 1) ^ 1) << 6);
}

const uint32_t *dkong_audio_debug_dac_hist(void) { return g_dac_hist; }

void dkong_audio_debug_take_requests(uint32_t *cmd_writes, uint32_t *irq_asserts,
                                     uint32_t *sig_bits, uint8_t *cmds_seen) {
    if (cmd_writes)  *cmd_writes  = g_cmd_writes;
    if (irq_asserts) *irq_asserts = g_irq_asserts;
    for (int i = 0; i < 8 && sig_bits; i++) sig_bits[i] = g_sig_bit_writes[i];
    for (int i = 0; i < 16 && cmds_seen; i++) cmds_seen[i] = g_cmd_seen[i];
    g_cmd_writes = g_irq_asserts = 0;
    for (int i = 0; i < 8; i++) g_sig_bit_writes[i] = 0;
}

void dkong_audio_debug_take_duty(uint32_t *bits_high, uint32_t *samples) {
    for (int i = 0; i < 8 && bits_high; i++) bits_high[i] = g_sig_bit_high[i];
    if (samples) *samples = g_sig_samples;
    for (int i = 0; i < 8; i++) g_sig_bit_high[i] = 0;
    g_sig_samples = 0;
}

void dkong_audio_debug_take_triggers(uint32_t *walk, uint32_t *jump, uint32_t *stomp) {
    if (walk)  *walk  = g_trig_walk;
    if (jump)  *jump  = g_trig_jump;
    if (stomp) *stomp = g_trig_stomp;
    g_trig_walk = g_trig_jump = g_trig_stomp = 0;
}

void dkong_audio_debug_take_dac(uint32_t *out_p1_writes, uint32_t *out_p2_writes,
                                uint8_t *out_dac_min, uint8_t *out_dac_max,
                                uint8_t *out_p2_value) {
    if (out_p2_value)  *out_p2_value  = g_p2_latch;
    if (out_p1_writes) *out_p1_writes = g_p1_writes;
    if (out_p2_writes) *out_p2_writes = g_p2_writes;
    if (out_dac_min)   *out_dac_min   = g_dac_min;
    if (out_dac_max)   *out_dac_max   = g_dac_max;
    g_p1_writes = g_p2_writes = 0;
    g_dac_min = 0xFF; g_dac_max = 0;
}

void dkong_audio_debug_take_stats(uint32_t *out_underruns, uint32_t *out_overruns,
                                  uint32_t *out_peak_depth, uint32_t *out_sound_cycles) {
    if (out_underruns)    *out_underruns    = g_underruns;
    if (out_overruns)     *out_overruns     = g_overruns;
    if (out_peak_depth)   *out_peak_depth   = g_peak_depth;
    if (out_sound_cycles) *out_sound_cycles = g_sound_cycles;
    g_underruns = g_overruns = g_peak_depth = g_sound_cycles = 0;
}

void dkong_audio_init(dkong_system *system) {
    (void)system;

    g_cmd_latch = 0;
    g_sig_latch = 0;
    g_p2_latch  = 0xFF;
    g_dac       = 0x80;
    g_dac_decay = 1.0f;
    g_dac_dc    = 0.0f;
    g_stomp_env = g_jump_env = g_walk_env = 0.0f;
    g_stomp_clk = g_stomp_lp = 0.0f; g_stomp_count = 0; g_stomp_prev = 0;
    g_walk_t = 0.0f;
    g_sk_z1 = g_sk_z2 = 0.0f;
    g_fifo_head = g_fifo_tail = 0;
    g_underruns = g_overruns = g_peak_depth = g_sound_cycles = 0;

    for (int i = 0; i < SINTAB_SIZE; i++)
        g_sintab[i] = sinf(6.2831853f * (float)i / (float)SINTAB_SIZE);

    sallen_key_init(1916.0f, 0.74f, (float)DKONG_AUDIO_SAMPLE_RATE);

    // MB8884: no internal ROM, 64 bytes of RAM (MAME's mb8884_device).
    g_cpu.program_r = snd_program_r;
    g_cpu.bus_r     = snd_bus_r;
    g_cpu.bus_w     = snd_bus_w;
    g_cpu.port_r    = snd_port_r;
    g_cpu.port_w    = snd_port_w;
    g_cpu.test_r    = snd_test_r;
    g_cpu.ext_r     = snd_ext_r;
    g_cpu.ext_w     = snd_ext_w;
    g_cpu.userdata  = NULL;
    mcs48_init(&g_cpu, 64);

    hal_audio_set_fill_callback(dkong_audio_fill);
}
