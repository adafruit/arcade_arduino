// Namco WSG (3-voice wavetable) synthesis.
//
// Register map and frequency-accumulator model verified against MAME's
// namco_wsg_device (src/devices/sound/namco.cpp/.h, Voices=3,
// Packed=false) as wired by Pac-Man's own `pacman_sound_w()`:
//
//   sound_regs[0x05]       ch0 waveform select (low 3 bits)
//   sound_regs[0x0A]       ch1 waveform select
//   sound_regs[0x0F]       ch2 waveform select
//   sound_regs[0x10]       ch0 frequency, extra low nibble (ch0 only --
//                          ch1/ch2 have no equivalent, so their frequency
//                          is always a multiple of 16)
//   sound_regs[0x11-0x14]  ch0 frequency nibbles (<<4, <<8, <<12, <<16)
//   sound_regs[0x15]       ch0 volume (low nibble)
//   sound_regs[0x16-0x19]  ch1 frequency nibbles (<<4, <<8, <<12, <<16)
//   sound_regs[0x1A]       ch1 volume
//   sound_regs[0x1B-0x1E]  ch2 frequency nibbles
//   sound_regs[0x1F]       ch2 volume
//
// (offsets above are relative to sound_regs[0] == Z80 address 0x5040;
// pacman_ports.cpp already masks each write to 0x0F on the way in, so no
// further masking is needed here.)
//
// Each voice is a free-running 20-bit phase accumulator, advanced every
// WSG clock tick (real hardware: 18.432MHz/6/32 = 96000 Hz, per MAME's
// `NAMCO_WSG(config, m_namco_sound, 18.432_MHz_XTAL/6/32)`) by its
// frequency register; the top 5 bits of that 20-bit accumulator
// (`(counter >> 15) & 0x1F`, per namco.h's `waveform_position()`) select
// which of the waveform's 32 samples to output. This file reimplements
// that same accumulator at PACMAN_AUDIO_SAMPLE_RATE instead of the real
// 96000 Hz base rate (the two are mathematically equivalent -- see the
// per-sample step derivation below), with 8 extra fractional bits of
// accumulator precision so low frequency registers (quiet, low-pitched
// tones) don't get rounded to a stuck or stepped pitch.
//
// Waveform sample lookup (non-packed, per namco_audio_device::
// waveform_r()): signed_sample = (pacman_wave_prom[select*32 + position]
// & 0x0F) - 8, range -8..7. `select` (0-7) and `position` (0-31) together
// span exactly the 256-byte 82s126.1m PROM.
#include "pacman_audio.h"
#include "arcade_hal_audio.h"

// __not_in_flash_func: same deliberate, isolated exception
// ArcadeMachine_Invaders's invaders_audio.cpp documents in full (see that
// file and arcade_arduino/DEVNOTES.md problem #7) -- pacman_audio_fill()
// below runs in the board's audio ISR and must never execute from flash.
#include "pico.h" // pulls in pico/platform.h (__not_in_flash_func) -- on
                   // RP2350, pico/platform.h refuses direct inclusion.

uint8_t pacman_wave_prom[PACMAN_WAVE_PROM_SIZE];

#define WSG_CLOCK_HZ 96000UL   // 18.432MHz / 6 / 32 -- see header comment
#define FRAC_BITS    8         // extra accumulator precision beyond the
                                // real chip's 20-bit phase register
#define POS_SHIFT    (15 + FRAC_BITS) // top 5 bits after the extra frac bits

// Output scale: each voice's raw sample is -8..7, times volume 0..15, so
// one voice's contribution is roughly -120..105; three voices summed is
// roughly -360..315. This constant maps that range up into a reasonable
// fraction of int16 headroom. Chosen empirically, not derived from any
// real hardware measurement -- adjust here if real hardware sounds too
// quiet/loud or clips (see CLAUDE.md: audio level is exactly the kind of
// thing that can only really be judged on real hardware).
#define OUTPUT_SCALE 48

static pacman_system *g_system;

static void __not_in_flash_func(pacman_audio_fill)(int32_t *out, int count) {
    uint8_t regs[PACMAN_SOUND_REG_SIZE];
    bool enabled;

    uint32_t saved = hal_audio_enter_critical();
    for (int i = 0; i < PACMAN_SOUND_REG_SIZE; i++) regs[i] = g_system->sound_regs[i];
    enabled = g_system->sound_enable;
    hal_audio_exit_critical(saved);

    if (!enabled) {
        for (int i = 0; i < count; i++) out[i] = 0;
        return;
    }

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

    // step = frequency * (WSG_CLOCK_HZ / PACMAN_AUDIO_SAMPLE_RATE), scaled
    // up by 2^FRAC_BITS for fixed-point precision. 64-bit intermediate
    // avoids any overflow risk (frequency is at most 20 bits).
    step[0] = (uint32_t)(((uint64_t)freq0 * WSG_CLOCK_HZ << FRAC_BITS) / PACMAN_AUDIO_SAMPLE_RATE);
    step[1] = (uint32_t)(((uint64_t)freq1 * WSG_CLOCK_HZ << FRAC_BITS) / PACMAN_AUDIO_SAMPLE_RATE);
    step[2] = (uint32_t)(((uint64_t)freq2 * WSG_CLOCK_HZ << FRAC_BITS) / PACMAN_AUDIO_SAMPLE_RATE);

    for (int i = 0; i < count; i++) {
        int mix = 0;
        for (int v = 0; v < 3; v++) {
            phase[v] += step[v];
            uint32_t pos = (phase[v] >> POS_SHIFT) & 0x1F;
            int sample = (int)(pacman_wave_prom[(waveform[v] << 5) | pos] & 0x0F) - 8;
            mix += sample * (int)volume[v];
        }
        int16_t s = (int16_t)(mix * OUTPUT_SCALE);
        out[i] = ((int32_t)s << 16) | (uint16_t)s;
    }
}

void pacman_audio_init(pacman_system *system) {
    g_system = system;
    hal_audio_set_fill_callback(pacman_audio_fill);
}
