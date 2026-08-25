// Space Invaders sample mixer -- ported from invaders_pico's pico_sound.c.
// Hardware bring-up (codec + I2S/DMA) is gone from this file; it lives in
// the board backend behind hal_audio_init(). This file keeps the WAV
// parsing, SRAM PCM copy, and the interrupt-safe channel mixer, which are
// all board-agnostic game/asset logic.
#include <stdio.h>
#include <string.h>
#include "invaders_audio.h"
#include "arcade_hal_audio.h"
#include "arcade_hal_storage.h"

// __not_in_flash_func: a deliberate, isolated exception to this file
// otherwise being board-agnostic. fill_audio_buffer() below runs in the
// board's audio ISR and must never execute from flash -- an XIP cache-miss
// stall there is long enough to starve a shared-timing-budget peripheral
// like PicoDVI (see invaders_pico's DEVNOTES.md "Red horizontal lines when
// sounds play" -- this exact bug, reproduced once already during this
// port). pico/platform.h is available on every realistic near-term board
// target for this framework (arduino-pico only targets the RP2040/RP2350
// family), so this is a pragmatic compromise rather than a generic
// portability abstraction; a board on a genuinely different toolchain
// would need an equivalent placement mechanism at this exact spot.
#include "pico.h" // pulls in pico/platform.h (__not_in_flash_func) -- on RP2350,
                   // pico/platform.h refuses direct inclusion and requires this

// MAME sample file index for each game sound slot.
// sample_map[game_slot] = mame_file_number (N in /samples/N.wav)
static const int sample_map[INVADERS_NUM_SAMPLES] = {0, 1, 2, 3, 9, 4, 5, 6, 7, 8};

// Q16.16 fixed-point position/step. step = (src_rate << 16) / output_rate
typedef struct {
    const uint8_t *pcm;
    uint32_t       pcm_bytes;
    uint32_t       pos;   // Q16.16 byte position
    uint32_t       step;  // Q16.16 step per output sample
    bool           is_16bit;
    bool           loop;  // restart instead of deactivating at end (UFO sound)
    bool           active;
} sound_channel_t;

#define MAX_CHANNELS 10
static sound_channel_t channels[MAX_CHANNELS];

// PCM data copied from storage to SRAM at init -- the mixer below runs in
// the board's audio ISR and must never read from flash/XIP (a cache-miss
// stall there is long enough to glitch a shared-timing-budget peripheral
// like PicoDVI; see invaders_pico's DEVNOTES.md "Red horizontal lines when
// sounds play"). 90000 bytes matches the reference clone's sizing for the
// stock Space Invaders sample set.
static uint8_t pcm_ram[90000];

// Temp buffer for loading one WAV file at a time during init.
static uint8_t wav_load_buf[32768];

typedef struct {
    const uint8_t *pcm;
    uint32_t       bytes;
    uint32_t       step;
    bool           is_16bit;
    bool           valid;
} wav_info_t;
static wav_info_t wav_info[INVADERS_NUM_SAMPLES];

static bool parse_wav(const uint8_t *data, size_t size, uint32_t out_sample_rate,
                       const uint8_t **pcm_out, uint32_t *bytes_out,
                       uint32_t *step_out, bool *is_16bit_out) {
    if (size < 12) return false;
    if (memcmp(data, "RIFF", 4) || memcmp(data + 8, "WAVE", 4)) return false;

    uint32_t sample_rate = 0;
    uint16_t bits = 8;
    const uint8_t *pcm = NULL;
    uint32_t pcm_bytes = 0;

    uint32_t off = 12;
    while (off + 8 <= (uint32_t)size) {
        uint32_t chunk_len = (uint32_t)data[off+4]
                           | ((uint32_t)data[off+5] << 8)
                           | ((uint32_t)data[off+6] << 16)
                           | ((uint32_t)data[off+7] << 24);
        if (!memcmp(data + off, "fmt ", 4) && chunk_len >= 16) {
            sample_rate = (uint32_t)data[off+12] | ((uint32_t)data[off+13] << 8)
                        | ((uint32_t)data[off+14] << 16) | ((uint32_t)data[off+15] << 24);
            bits = (uint16_t)data[off+22] | ((uint16_t)data[off+23] << 8);
        } else if (!memcmp(data + off, "data", 4)) {
            pcm       = data + off + 8;
            pcm_bytes = chunk_len;
            break;
        }
        off += 8 + ((chunk_len + 1u) & ~1u);
    }
    if (!pcm || !sample_rate) return false;

    *pcm_out      = pcm;
    *bytes_out    = pcm_bytes;
    *is_16bit_out = (bits == 16);
    *step_out     = (uint32_t)(((uint64_t)sample_rate << 16) / out_sample_rate);
    return true;
}

// Runs in the board's audio ISR/DMA-completion handler (registered below
// via hal_audio_set_fill_callback) -- must stay in RAM, no flash/XIP reads.
static void __not_in_flash_func(fill_audio_buffer)(int32_t *buf, int count) {
    for (int i = 0; i < count; i++) {
        int32_t mix = 0;
        for (int c = 0; c < MAX_CHANNELS; c++) {
            if (!channels[c].active) continue;
            uint32_t byte_idx = channels[c].pos >> 16;
            if (channels[c].is_16bit) {
                byte_idx &= ~1u;
                if (byte_idx + 2 > channels[c].pcm_bytes) {
                    if (channels[c].loop) { channels[c].pos = 0; }
                    else { channels[c].active = false; }
                    continue;
                }
                int16_t s = (int16_t)((uint16_t)channels[c].pcm[byte_idx]
                          | ((uint16_t)channels[c].pcm[byte_idx + 1] << 8));
                mix += s;
            } else {
                if (byte_idx >= channels[c].pcm_bytes) {
                    if (channels[c].loop) { channels[c].pos = 0; }
                    else { channels[c].active = false; }
                    continue;
                }
                mix += ((int32_t)channels[c].pcm[byte_idx] - 128) << 8;
            }
            channels[c].pos += channels[c].step;
        }
        if (mix >  32767) mix =  32767;
        if (mix < -32768) mix = -32768;
        int16_t s = (int16_t)mix;
        buf[i] = ((int32_t)s << 16) | (uint16_t)s;
    }
}

int invaders_audio_load_samples(void) {
    memset(channels, 0, sizeof(channels));
    uint32_t ram_off = 0;
    int loaded = 0;

    for (int i = 0; i < INVADERS_NUM_SAMPLES; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/samples/%d.wav", sample_map[i]);

        hal_file_t *f = hal_storage_open(path);
        if (f) {
            uint32_t bytes_read = hal_storage_read(f, wav_load_buf, sizeof(wav_load_buf));
            hal_storage_close(f);

            if (bytes_read > 0) {
                wav_info[i].valid = parse_wav(
                    wav_load_buf, (size_t)bytes_read, INVADERS_AUDIO_SAMPLE_RATE,
                    &wav_info[i].pcm, &wav_info[i].bytes,
                    &wav_info[i].step, &wav_info[i].is_16bit);
                if (wav_info[i].valid && ram_off + wav_info[i].bytes <= sizeof(pcm_ram)) {
                    memcpy(pcm_ram + ram_off, wav_info[i].pcm, wav_info[i].bytes);
                    wav_info[i].pcm = pcm_ram + ram_off;
                    ram_off += wav_info[i].bytes;
                    loaded++;
                }
            }
        }
        // If a sample file is missing, wav_info[i].valid stays false -> silent.
    }

    hal_audio_set_fill_callback(fill_audio_buffer);
    return loaded;
}

void invaders_audio_play(int sample_num) {
    if (sample_num < 0 || sample_num >= INVADERS_NUM_SAMPLES) return;
    if (!wav_info[sample_num].valid) return;

    // Find a free channel; if all busy, reuse the one furthest along.
    int slot = 0;
    uint32_t max_pos = 0;
    bool found = false;
    for (int c = 0; c < MAX_CHANNELS && !found; c++) {
        if (!channels[c].active) { slot = c; found = true; break; }
        if (channels[c].pos > max_pos) { max_pos = channels[c].pos; slot = c; }
    }

    sound_channel_t next = {};
    next.pcm       = wav_info[sample_num].pcm;
    next.pcm_bytes = wav_info[sample_num].bytes;
    next.step      = wav_info[sample_num].step;
    next.is_16bit  = wav_info[sample_num].is_16bit;
    next.pos       = 0;
    next.loop      = (sample_num == 0); // UFO flight loops until channel is stolen
    next.active    = true;

    uint32_t saved = hal_audio_enter_critical();
    channels[slot] = next;
    hal_audio_exit_critical(saved);
}

void invaders_audio_stop(int sample_num) {
    if (sample_num < 0 || sample_num >= INVADERS_NUM_SAMPLES) return;
    if (!wav_info[sample_num].valid) return;
    uint32_t saved = hal_audio_enter_critical();
    for (int c = 0; c < MAX_CHANNELS; c++) {
        if (channels[c].active && channels[c].pcm == wav_info[sample_num].pcm)
            channels[c].active = false;
    }
    hal_audio_exit_critical(saved);
}
