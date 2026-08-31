// Namco WSG (3-voice wavetable) synthesis for Galaga.
//
// See galaga_audio.h for why this deliberately mirrors
// ArcadeMachine_Pacman's pacman_audio.cpp (same chip, same register map,
// same 96000Hz clock -- MAME literally routes Galaga's writes through
// Pac-Man's own `pacman_sound_w` handler) and for the two real differences
// (no sound-enable latch; different waveform PROM). The register-map and
// phase-accumulator derivation lives in pacman_audio.cpp's header comment
// and is not repeated here; the register offsets below are relative to
// wsg_regs[0] == Z80 address 0x6800.
//
//   wsg_regs[0x05]       ch0 waveform select (low 3 bits)
//   wsg_regs[0x0A]       ch1 waveform select
//   wsg_regs[0x0F]       ch2 waveform select
//   wsg_regs[0x10]       ch0 frequency, extra low nibble (ch0 only)
//   wsg_regs[0x11-0x14]  ch0 frequency nibbles (<<4, <<8, <<12, <<16)
//   wsg_regs[0x15]       ch0 volume (low nibble)
//   wsg_regs[0x16-0x19]  ch1 frequency nibbles
//   wsg_regs[0x1A]       ch1 volume
//   wsg_regs[0x1B-0x1E]  ch2 frequency nibbles
//   wsg_regs[0x1F]       ch2 volume
//
// galaga_ports.cpp already masks every write to 0x0F on the way in, so no
// further masking is needed here.
#include "galaga_audio.h"
#include "galaga_54xx.h"
#include "arcade_hal_audio.h"

// __not_in_flash_func: the same deliberate, isolated exception
// ArcadeMachine_Invaders's invaders_audio.cpp documents in full (see that
// file and arcade_arduino/DEVNOTES.md problem #7) -- galaga_audio_fill()
// runs in the board's audio ISR and must never execute from flash, where
// an XIP cache miss would stall it. (This machine leans on that mechanism
// elsewhere too: see galaga_machine.cpp's GALAGA_M_RAMFUNC.)
#include <Arduino.h> // micros() for the ISR instrument below
#include "pico.h" // pulls in pico/platform.h (__not_in_flash_func) -- on
                   // RP2350, pico/platform.h refuses direct inclusion.

uint8_t galaga_wave_prom[GALAGA_WAVE_PROM_SIZE];

#define WSG_CLOCK_HZ 96000UL   // 18.432MHz / 6 / 32, per galaga()'s NAMCO_WSG()
#define FRAC_BITS    8         // extra accumulator precision beyond the
                                // real chip's 20-bit phase register
#define POS_SHIFT    (15 + FRAC_BITS) // top 5 bits after the extra frac bits

// Output scale: each voice's raw sample is -8..7, times volume 0..15, so
// one voice contributes roughly -120..105 and three summed roughly
// -360..315. Same starting value Pac-Man uses, for the same reason -- it
// maps that range into a sensible fraction of int16 headroom. Empirical,
// not derived from a hardware measurement: adjust here if Galaga turns out
// too quiet/loud or clips on real hardware (per CLAUDE.md, audio level is
// exactly the sort of thing only a real board can settle). Note MAME mixes
// Galaga's WSG at 0.90*10/16 of full scale, a little under Pac-Man's, so
// if anything this may want to come DOWN rather than up.
#define OUTPUT_SCALE 48

static galaga_system *g_system;

// ISR cost instrumentation. DEVNOTES.md problem #35 named this as the next
// thing to measure and the reason: this is the only audio ISR in the project
// never instrumented, and the heaviest per sample (three WSG voices plus the
// 54XX's two layered noise voices, each with three filter bands and a
// sample-and-hold). Everything else about #35 was ruled out by measurement;
// this was ruled out by nothing.
//
// Modelled on lrescue_audio_debug_isr_stats(), which #35 points at. The
// counters are plain uint32_t written only here and read with interrupts
// masked by the accessor, so no critical section is needed on this side.
static uint32_t g_isr_us, g_isr_calls, g_isr_max_us;

void galaga_audio_debug_take_isr_stats(uint32_t *total_us, uint32_t *calls,
                                       uint32_t *max_call_us) {
    if (total_us)    *total_us    = g_isr_us;
    if (calls)       *calls       = g_isr_calls;
    if (max_call_us) *max_call_us = g_isr_max_us;
    g_isr_us = g_isr_calls = g_isr_max_us = 0;
}

static void __not_in_flash_func(galaga_audio_fill)(int32_t *out, int count) {
    uint32_t isr_t0 = micros();
    uint8_t regs[0x20];

    uint32_t saved = hal_audio_enter_critical();
    for (int i = 0; i < 0x20; i++) regs[i] = g_system->wsg_regs[i];
    // Hand over any pending 54XX play command in the same critical section
    // -- the CPU sets it, this ISR consumes it (see galaga_54xx.h).
    galaga_54xx_take_trigger(&g_system->io54);
    hal_audio_exit_critical(saved);

    // No sound-enable gate here, unlike Pac-Man: Galaga's misclatch has no
    // bit routed to the sound device (see galaga_audio.h).

    static uint32_t phase[3] = {0, 0, 0};

    uint8_t  waveform[3];
    uint8_t  volume[3];
    uint32_t step[3];

    waveform[0] = regs[0x05] & 0x07;
    waveform[1] = regs[0x0A] & 0x07;
    waveform[2] = regs[0x0F] & 0x07;

    volume[0] = regs[0x15] & 0x0F;
    volume[1] = regs[0x1A] & 0x0F;
    volume[2] = regs[0x1F] & 0x0F;

    uint32_t freq0 = (uint32_t)regs[0x10]
        | ((uint32_t)regs[0x11] << 4) | ((uint32_t)regs[0x12] << 8)
        | ((uint32_t)regs[0x13] << 12) | ((uint32_t)regs[0x14] << 16);
    uint32_t freq1 = ((uint32_t)regs[0x16] << 4) | ((uint32_t)regs[0x17] << 8)
        | ((uint32_t)regs[0x18] << 12) | ((uint32_t)regs[0x19] << 16);
    uint32_t freq2 = ((uint32_t)regs[0x1B] << 4) | ((uint32_t)regs[0x1C] << 8)
        | ((uint32_t)regs[0x1D] << 12) | ((uint32_t)regs[0x1E] << 16);

    // step = frequency * (WSG_CLOCK_HZ / SAMPLE_RATE), scaled by 2^FRAC_BITS.
    // 64-bit intermediate avoids overflow (frequency is at most 20 bits).
    step[0] = (uint32_t)(((uint64_t)freq0 * WSG_CLOCK_HZ << FRAC_BITS) / GALAGA_AUDIO_SAMPLE_RATE);
    step[1] = (uint32_t)(((uint64_t)freq1 * WSG_CLOCK_HZ << FRAC_BITS) / GALAGA_AUDIO_SAMPLE_RATE);
    step[2] = (uint32_t)(((uint64_t)freq2 * WSG_CLOCK_HZ << FRAC_BITS) / GALAGA_AUDIO_SAMPLE_RATE);

    for (int i = 0; i < count; i++) {
        int mix = 0;
        for (int v = 0; v < 3; v++) {
            phase[v] += step[v];
            uint32_t pos = (phase[v] >> POS_SHIFT) & 0x1F;
            int sample = (int)(galaga_wave_prom[(waveform[v] << 5) | pos] & 0x0F) - 8;
            mix += sample * (int)volume[v];
        }
        // WSG voices plus the 54XX explosion/noise channel, which is a
        // separate chip on the real board and mixes in alongside them.
        int32_t total = mix * OUTPUT_SCALE + galaga_54xx_sample(&g_system->io54);
        if (total >  32767) total =  32767; // the 54XX burst can peak on top
        if (total < -32768) total = -32768; // of a loud chord -- clamp, don't wrap
        int16_t s = (int16_t)total;
        out[i] = ((int32_t)s << 16) | (uint16_t)s;
    }

    uint32_t isr_dt = micros() - isr_t0;
    g_isr_us += isr_dt;
    g_isr_calls++;
    if (isr_dt > g_isr_max_us) g_isr_max_us = isr_dt;
}

void galaga_audio_init(galaga_system *system) {
    g_system = system;
    hal_audio_set_fill_callback(galaga_audio_fill);
}
