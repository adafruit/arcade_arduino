// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Lunar Rescue sample mixer -- the interrupt-safe channel mixer itself is
// structurally a copy of ArcadeMachine_Invaders' invaders_audio.cpp, plus a
// bit-banged square-wave channel for the one sound this game synthesizes
// instead of sampling (see lrescue_audio.h). The WAV loading path is NOT a
// copy, though: Invaders' samples fit its 90000-byte SRAM budget as
// captured, but Lunar Rescue's real sample set is ~895KB of 44.1kHz 16-bit
// PCM, several files larger than any reasonable single scratch buffer.
// lrescue_audio_load_samples() below streams each file in bounded chunks
// (never the whole file in RAM at once) through a nearest-neighbor
// resampler that downconverts to 8-bit mono at LRESCUE_AUDIO_SAMPLE_RATE on
// the way into pcm_ram -- see stream_convert_to_pcm_ram()'s comment for
// the real numbers this was sized against.
#include <stdio.h>
#include <string.h>
#include "lrescue_audio.h"
#include "lrescue_machine.h" // LRESCUE_CPU_HZ -- see fill_audio_buffer()'s use of it below
#include "arcade_hal_audio.h"
#include "arcade_hal_storage.h"

// DEBUG: real serial diagnostics, since color-coded flashes have reached
// the limit of what they can tell us -- this reports exact numbers instead
// of a bucketed red/green guess. May simply produce nothing: this board's
// USB may be wired to a PIO host peripheral rather than a device link to a
// PC (see CLAUDE.md); worth trying anyway since it costs nothing if so.
#include <Arduino.h>

// See invaders_audio.cpp for why this include and __not_in_flash_func exist:
// fill_audio_buffer() runs in the board's audio ISR and must never execute
// from flash -- an XIP cache-miss stall there is long enough to starve the
// PicoDVI scanline queue (invaders_pico's DEVNOTES.md "Red horizontal lines
// when sounds play").
#include "pico.h"
#include "pico/time.h"     // time_us_64() -- see lrescue_audio_now_cycles() below (Core-0-normal-path only, NOT the ISR)
#include "hardware/timer.h" // timer_hw->timerawl -- see g_isr_time_us_accum's doc comment below for why the ISR uses this instead

// LRESCUE_CPU_HZ/LRESCUE_AUDIO_SAMPLE_RATE as an exact integer-plus-
// remainder split, both halves compile-time constants (all four operands
// are literal macros, so the compiler folds these divisions itself --
// nothing here runs at runtime, let alone in the ISR). Used both by
// fill_audio_buffer()'s carry-remainder cycle accumulator and by
// lrescue_audio_speaker_event()'s producer-side decimation below --
// LRESCUE_CYCLES_PER_SAMPLE_INT is "how many i8080 cycles wide is one
// output audio sample", i.e. the finest cycle resolution this channel can
// actually represent.
#define LRESCUE_CYCLES_PER_SAMPLE_INT  ((uint64_t)(LRESCUE_CPU_HZ) / (uint64_t)LRESCUE_AUDIO_SAMPLE_RATE)
#define LRESCUE_CYCLES_PER_SAMPLE_FRAC ((uint64_t)(LRESCUE_CPU_HZ) % (uint64_t)LRESCUE_AUDIO_SAMPLE_RATE)

typedef struct {
    const uint8_t *pcm;
    uint32_t       pcm_bytes;
    uint32_t       pos;   // Q16.16 byte position
    uint32_t       step;  // Q16.16 step per output sample
    bool           is_16bit;
    bool           loop;  // restart instead of deactivating at end (shooting star/rescue ship)
    bool           active;
} sound_channel_t;

#define MAX_CHANNELS 10
static sound_channel_t channels[MAX_CHANNELS];

// How many of the MAX_CHANNELS slots lrescue_audio_play() is willing to
// actually use at once -- deliberately smaller than MAX_CHANNELS. Measured
// on real hardware (lrescue_audio_debug_isr_stats(), during the red-
// scanline investigation): fill_audio_buffer()'s own per-invocation cost
// scales with how many channels are concurrently active (avg ~146-158us at
// 0 active, up to ~209us avg / 248us worst-single at 5) -- a real, if
// modest, contribution to Core 0's time on top of everything else already
// addressed (FRAMERATE calibration, the render_scanline() optimization).
// Capping concurrency bounds that contribution's worst case directly,
// rather than just hoping fewer channels happen to be active when it
// matters. 4 was picked as a middle ground: still leaves room for several
// genuinely distinct simultaneous effects (this game's own real high-
// concurrency moments, e.g. two players shooting during a rescue, rarely
// need more before an older sound is naturally finishing anyway), while
// meaningfully trimming the tail this array's full 10 slots allowed. The
// channel-stealing logic below (reuse whichever active channel is furthest
// along) kicks in at this smaller number instead of at MAX_CHANNELS, so a
// 5th+ concurrent sound reuses an already-mostly-finished channel rather
// than being silently dropped.
#define MAX_CONCURRENT_CHANNELS 4

// PCM data copied from storage to SRAM at init -- must never be read from
// flash/XIP inside the audio ISR (see above).
//
// Unlike Invaders' 90000-byte budget, this can't just be "generous": Lunar
// Rescue's actual sample set (as recorded/captured, 44.1kHz 16-bit mono) is
// ~895KB of PCM data -- nowhere close to fitting in SRAM at that
// resolution, and several individual files exceed what used to be this
// loader's whole per-file scratch buffer. lrescue_audio_load_samples()
// below downsamples every file to 8-bit mono at LRESCUE_AUDIO_SAMPLE_RATE
// while streaming it in from storage (never holding a whole file in RAM at
// once), which brings the real total down to ~224000 bytes; this budget
// leaves ~10% headroom above that for slightly longer replacement assets.
static uint8_t pcm_ram[245760];

// Bounded staging buffers for the streaming WAV loader -- deliberately NOT
// sized to fit a whole file (see above). header_buf holds enough of the
// file's start to parse RIFF/fmt/data chunk headers (and may already
// contain the first slice of PCM data too, for small files); chunk_buf is
// reused across every subsequent sequential read of the rest of the data
// chunk.
#define WAV_HEADER_BUF_SIZE 1024
#define WAV_CHUNK_BUF_SIZE  4096
static uint8_t header_buf[WAV_HEADER_BUF_SIZE];
static uint8_t chunk_buf[WAV_CHUNK_BUF_SIZE];

typedef struct {
    const uint8_t *pcm;
    uint32_t       bytes;
    uint32_t       step;
    bool           is_16bit;
    bool           valid;
} wav_info_t;
static wav_info_t wav_info[LRESCUE_NUM_SAMPLES];

// MAME's own lrescue_sample_names[] (midw8080/8080bw_a.cpp), used verbatim
// as filenames so a stock MAME "lrescue" sample pack drops in unmodified.
static const char *const SAMPLE_FILENAMES[LRESCUE_NUM_SAMPLES] = {
    "alienexplosion", "rescueshipexplosion", "beamgun", "thrust",
    "bonus2", "bonus3", "shootingstar", "stepl", "steph",
};

static volatile bool g_muted = false;
// Chosen to sit comfortably under int16 mix headroom alongside up to a
// handful of concurrent samples; matches MAME's relative mix weighting
// (speaker at 0.25 vs samples at 0.75) only approximately -- there's no
// hardware reference recording to calibrate against, so treat this as a
// starting point to tune by ear once real WAV assets are in place.
#define SPEAKER_AMPLITUDE 8000

// Cycle-timestamped speaker-level transitions, produced by
// lrescue_audio_speaker_event() (called from lrescue_ports.cpp, running as
// part of normal Core 0 execution -- NOT inside the audio ISR) and consumed
// by fill_audio_buffer() below (which IS the audio ISR, and can preempt the
// producer at any point). Single-producer/single-consumer ring buffer:
// only the producer ever advances `head`, only the consumer ever advances
// `tail`, so the consumer needs no locking against itself, but the
// producer's multi-step write (fill the slot, then publish by advancing
// head) is wrapped in hal_audio_enter_critical()/exit_critical() so the
// ISR can never observe a half-written slot.
//
// 256 entries is generous headroom: even a fast musical passage toggling
// in the low kHz would only produce on the order of tens of transitions
// within one video frame's ~16.67ms (the window that can accumulate before
// the ISR gets a chance to drain any of it -- see lrescue_audio_h's doc
// comment on lrescue_audio_speaker_event() for why frames are the relevant
// unit here). If it ever does fill up, new events are dropped rather than
// overwriting unread ones or blocking Core 0.
#define SPEAKER_EVENT_QUEUE_SIZE 256
typedef struct { uint64_t cycle; bool level; } speaker_event_t;
static speaker_event_t speaker_events[SPEAKER_EVENT_QUEUE_SIZE];
static volatile uint32_t speaker_events_head = 0; // next write slot (producer-owned)
static volatile uint32_t speaker_events_tail = 0; // next unread slot (consumer-owned)
static bool speaker_last_level = false; // level in effect before the oldest still-queued event

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits;          // 8 or 16
    uint32_t data_bytes;    // total PCM byte count declared by the "data" chunk
    uint32_t header_pcm_off; // offset within header_buf where PCM data starts
    uint32_t header_pcm_len; // how many PCM bytes are already sitting in header_buf
} wav_fmt_t;

// Parses RIFF/fmt/data chunk headers out of the first `size` bytes already
// read into `buf` (header_buf). Does not read any more PCM data itself --
// just locates where it starts and how much of it (if any) is already
// sitting in `buf` past the "data" chunk header, so a small file whose PCM
// data is fully captured within header_buf is recognized as such.
static bool parse_wav_header(const uint8_t *buf, uint32_t size, wav_fmt_t *out) {
    if (size < 12) return false;
    if (memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) return false;

    bool have_fmt = false;
    uint32_t off = 12;
    while (off + 8 <= size) {
        uint32_t chunk_len = (uint32_t)buf[off+4]  | ((uint32_t)buf[off+5] << 8)
                           | ((uint32_t)buf[off+6] << 16) | ((uint32_t)buf[off+7] << 24);
        if (!have_fmt && !memcmp(buf + off, "fmt ", 4) && chunk_len >= 16) {
            out->channels    = (uint16_t)buf[off+10] | ((uint16_t)buf[off+11] << 8);
            out->sample_rate = (uint32_t)buf[off+12] | ((uint32_t)buf[off+13] << 8)
                             | ((uint32_t)buf[off+14] << 16) | ((uint32_t)buf[off+15] << 24);
            out->bits        = (uint16_t)buf[off+22] | ((uint16_t)buf[off+23] << 8);
            have_fmt = true;
        } else if (!memcmp(buf + off, "data", 4)) {
            if (!have_fmt) return false;
            out->data_bytes     = chunk_len;
            out->header_pcm_off = off + 8;
            uint32_t avail = size - out->header_pcm_off;
            out->header_pcm_len = avail < chunk_len ? avail : chunk_len;
            return true;
        }
        off += 8 + ((chunk_len + 1u) & ~1u);
    }
    return false; // "data" chunk header didn't fall within WAV_HEADER_BUF_SIZE bytes
}

// Reads one frame at `idx` (0-based) from a buffer holding raw PCM in
// `fmt`'s format, downmixed to a single signed-16 sample if the source is
// multi-channel (all of Lunar Rescue's own captured samples are mono, but
// this stays correct for a stereo replacement file too).
static inline int16_t read_frame_mono16(const uint8_t *src, uint32_t idx, const wav_fmt_t *fmt) {
    uint32_t bytes_per_sample = fmt->bits / 8;
    uint32_t frame_off = idx * bytes_per_sample * fmt->channels;
    int32_t acc = 0;
    for (uint16_t c = 0; c < fmt->channels; c++) {
        uint32_t s_off = frame_off + c * bytes_per_sample;
        int32_t sample;
        if (fmt->bits == 16) {
            sample = (int16_t)((uint16_t)src[s_off] | ((uint16_t)src[s_off + 1] << 8));
        } else {
            sample = ((int32_t)src[s_off] - 128) << 8; // 8-bit source is unsigned
        }
        acc += sample;
    }
    return (int16_t)(acc / fmt->channels);
}

// Streams the rest of one WAV file's PCM data (continuing sequentially from
// wherever the header read left the file's cursor) through a nearest-
// neighbor resampler straight into pcm_ram, converting to 8-bit unsigned
// mono at LRESCUE_AUDIO_SAMPLE_RATE as it goes -- never holding more than
// WAV_CHUNK_BUF_SIZE bytes of source data in RAM at once, regardless of how
// large the source file is. Stops early (returning whatever was written so
// far) if pcm_ram fills up, rather than overflowing it.
// Real files here top out at bonus3.wav's ~61778 converted bytes; this
// gives headroom above that while staying under 65536 -- playback
// (fill_audio_buffer, below) tracks position with the exact same uint32_t
// Q16.16 scheme this file's load-time bug turned out to be, so a sample
// this cap ever actually let through at >=65536 bytes would just move the
// same wraparound bug from loading to playback. NOT a fix for the actual
// bug (a file's conversion still shouldn't be able to run past its own
// true length at all -- see the caller-side diagnostics this was found
// with), but it firewalls the *consequence*: one file misbehaving can no
// longer cannibalize the rest of pcm_ram and starve every file loaded
// after it, which is what turned a single-file bug into "most of the
// sample set silently fails" every time it triggered.
#define MAX_OUTPUT_BYTES_PER_FILE 65000

static uint32_t stream_convert_to_pcm_ram(hal_file_t *f, const wav_fmt_t *fmt, uint32_t *ram_off) {
    if (fmt->sample_rate == 0 || fmt->channels == 0 || (fmt->bits != 8 && fmt->bits != 16))
        return 0;
    uint32_t bytes_per_frame = (fmt->bits / 8) * fmt->channels;
    if (bytes_per_frame == 0) return 0;
    uint32_t total_frames = fmt->data_bytes / bytes_per_frame;
    if (total_frames == 0) return 0;

    uint32_t step = (uint32_t)(((uint64_t)fmt->sample_rate << 16) / LRESCUE_AUDIO_SAMPLE_RATE);
    // Q16.16, but held in a uint64_t -- a uint32_t Q16.16 position only has
    // 16 INTEGER bits, so it silently wraps once the source-frame count it
    // needs to represent reaches 65536 (2^16): pos itself would need to
    // reach 65536<<16 == 2^32, one bit past what a uint32_t holds. Every
    // file here under that many frames (bonus2 through beamgun) converted
    // perfectly; every file over it (shootingstar, rescueshipexplosion,
    // thrust, bonus3 -- all real files, no card/read corruption at all,
    // confirmed via Serial logging) ran forever because `pos >> 16` kept
    // cycling back through 0 and never once reached (let alone exceeded)
    // total_frames. A uint64_t has 48 integer bits at this Q16.16 scale --
    // comfortably enough for any real WAV file.
    uint64_t pos = 0; // Q16.16 source-frame position, monotonic across the whole file
    uint32_t out_written = 0;

    const uint8_t *cur    = header_buf + fmt->header_pcm_off; // starts with whatever the header read already captured
    uint32_t cur_frames   = fmt->header_pcm_len / bytes_per_frame;
    uint32_t base_frame   = 0; // index of cur's first frame within the whole stream

    bool more = true;
    while (more) {
        while ((pos >> 16) < base_frame + cur_frames && (pos >> 16) < total_frames) {
            if (*ram_off >= sizeof(pcm_ram) || out_written >= MAX_OUTPUT_BYTES_PER_FILE) { more = false; break; }
            uint32_t local_idx = (pos >> 16) - base_frame;
            int16_t s16 = read_frame_mono16(cur, local_idx, fmt);
            pcm_ram[(*ram_off)++] = (uint8_t)(((int32_t)s16 >> 8) + 128);
            out_written++;
            pos += step;
        }
        if (!more || (pos >> 16) >= total_frames) break;

        base_frame += cur_frames;
        uint32_t frames_left = total_frames - base_frame;
        if (frames_left == 0) break;
        uint32_t chunk_frames = WAV_CHUNK_BUF_SIZE / bytes_per_frame;
        if (chunk_frames > frames_left) chunk_frames = frames_left;
        uint32_t want = chunk_frames * bytes_per_frame;
        uint32_t got = hal_storage_read(f, chunk_buf, want);
        if (got < bytes_per_frame) break; // short/truncated file -- stop with what we have
        cur = chunk_buf;
        cur_frames = got / bytes_per_frame;
    }
    return out_written;
}

// Drains any queued transitions whose cycle has now passed (relative to
// `target_cycle`), updating speaker_last_level to the most recent one, then
// returns the level in effect AT target_cycle. Called once per output
// sample from the audio ISR below -- this IS the consumer, so no locking
// is needed here (only the producer's writes need the critical section;
// see lrescue_audio_speaker_event()).
//
// Bounded to at most DRAIN_LIMIT iterations per call: lrescue_audio_speaker_
// event()'s decimation keeps this at 0-1 in steady state, but this cap is
// the actual guarantee -- with it, this function's worst-case cost is a
// small fixed constant no matter what arrives, which is what an ISR needs.
// Losing precision here only matters for content denser than one
// transition per output sample, which is already past this channel's
// Nyquist limit and would alias into noise regardless of how exactly it's
// reconstructed -- capping the drain doesn't make that any worse.
// DEBUG: lifetime ring-buffer health counters -- see lrescue_audio.h's doc
// comment on lrescue_audio_speaker_debug_stats() for what each means.
static volatile uint32_t g_speaker_events_pushed_total = 0;
static volatile uint32_t g_speaker_events_dropped_total = 0;
static volatile uint32_t g_speaker_queue_peak_depth = 0;
static volatile uint32_t g_speaker_drain_limit_hits = 0;

static inline bool __not_in_flash_func(speaker_level_at)(uint64_t target_cycle) {
    const int DRAIN_LIMIT = 8;
    int n;
    for (n = 0; n < DRAIN_LIMIT
         && speaker_events_tail != speaker_events_head
         && speaker_events[speaker_events_tail].cycle <= target_cycle; n++) {
        speaker_last_level = speaker_events[speaker_events_tail].level;
        speaker_events_tail = (speaker_events_tail + 1) % SPEAKER_EVENT_QUEUE_SIZE;
    }
    // DEBUG: hit the cap while an eligible-to-drain event still remained --
    // genuinely falling behind this call, not just "had a few queued".
    if (n == DRAIN_LIMIT && speaker_events_tail != speaker_events_head
        && speaker_events[speaker_events_tail].cycle <= target_cycle) {
        g_speaker_drain_limit_hits++;
    }
    return speaker_last_level;
}

// g_target_cycle tracks "what i8080 cycle count does the sample about to be
// generated correspond to" -- the same cycle axis lrescue_audio_speaker_
// event() timestamps port writes against (see that function's doc comment
// for why this, and not live polling, is what makes a bit-banged tune
// reconstruct correctly). Promoted from a function-local static (it only
// needs to live across fill_audio_buffer() calls) to file scope purely so
// lrescue_audio_debug_target_cycle() below can read it -- no behavior
// change.
static volatile uint64_t g_target_cycle = 0;
static uint32_t g_target_cycle_frac = 0; // in units where the denominator is LRESCUE_AUDIO_SAMPLE_RATE

// DEBUG: current value of the audio ISR's own cycle clock -- lets a sketch
// compare "what cycle is the mixer actually reconstructing right now" against
// the cycle values it's timestamping speaker events with (e.g. system-
// >total_cycles, or fake_cycles() in an isolation test). A large or growing
// gap between this and the most recent pushed cycle means the mixer is
// running behind (or ahead of) where events think "now" is -- the two are
// driven by independent clocks (the audio sample rate vs. whatever clocks
// the producer), so this is the direct way to check they're actually in
// sync instead of inferring it from what the speaker audibly does. Safe to
// delete once the red-scanline/crumbly-audio investigation is resolved.
uint64_t lrescue_audio_debug_target_cycle(void) {
    return g_target_cycle;
}

// Real-time epoch for lrescue_audio_now_cycles() below -- captured in
// lrescue_audio_load_samples() at essentially the same moment g_target_cycle
// starts counting from 0 (right where the audio ISR gets registered), so
// the two clocks agree on "now" from the start instead of one of them
// carrying a startup head-start the other doesn't (see this function's own
// doc comment in lrescue_audio.h for the actual bug this fixes).
static uint64_t g_audio_epoch_us = 0;

uint64_t lrescue_audio_now_cycles(void) {
    uint64_t elapsed_us = time_us_64() - g_audio_epoch_us;
    // Plain multiply+divide -- fine here (unlike inside fill_audio_buffer):
    // this runs on Core 0's normal call path from lrescue_ports.cpp, not
    // the audio ISR, so a software-division XIP call costs nothing this
    // investigation has already shown matters (CPU-loop time was measured
    // with headroom to spare; see lrescue_machine.cpp's cpu_record).
    return (elapsed_us * (uint64_t)(LRESCUE_CPU_HZ)) / 1000000ULL;
}

// DEBUG: this ISR's own cost, and how many channels were concurrently
// active while it ran -- see lrescue_audio_debug_isr_stats()'s doc comment
// (declared in lrescue_audio.h) for why this matters: it runs on Core 0,
// can preempt render/CPU-loop/blocked-on-video-queue work at any point, and
// its own per-call cost scales with active-channel count (the per-sample
// mixing loop below).
//
// IMPORTANT: measuring this call's own duration must NOT use time_us_64()
// (or time_us_32()) -- checked via `nm` on the linked .elf, both turned out
// to be real, separately-compiled pico-sdk functions placed in FLASH
// (0x10xxxxxx, not the 0x20xxxxxx SRAM range __not_in_flash_func()'d code
// like this ISR lives in -- confirmed by comparing against
// dvi_dma_irq_handler's known-RAM address), NOT the trivial inline they're
// often assumed to be. Calling either from here would silently reintroduce
// exactly the XIP-call-inside-the-audio-ISR bug class this whole
// investigation exists to avoid -- an early version of this instrumentation
// did exactly that before being caught. timer_hw->timerawl (from
// hardware/timer.h) is what those functions themselves read internally: a
// direct MMIO register load, no function call at all, genuinely RAM-safe.
// 32 bits (microseconds, wraps every ~71 minutes) is plenty for timing a
// single ISR invocation -- unsigned subtraction handles the wrap correctly
// regardless.
static volatile uint32_t g_isr_time_us_accum = 0;
static volatile uint32_t g_isr_max_single_call_us = 0; // DEBUG: worst ONE invocation, not an average
static volatile uint32_t g_isr_max_active_channels = 0;
static volatile uint32_t g_isr_invocation_count = 0;

// Runs in the board's audio ISR/DMA-completion handler -- must stay in RAM,
// no flash/XIP reads.
static void __not_in_flash_func(fill_audio_buffer)(int32_t *buf, int count) {
    // This used to be computed fresh each sample via a 64-bit multiply and
    // DIVIDE. The RP2350's Cortex-M33 has no hardware 64-bit divide, so
    // that called a software division routine likely living in flash --
    // an XIP read from inside the audio ISR, the exact class of bug
    // DEVNOTES.md documents ("red horizontal lines when sounds play").
    // That was worth fixing regardless, but on its own it did NOT fully
    // resolve the red-scanline symptom this comment was originally
    // written to explain -- see the SPEAKER_DEBUG_SILENT experiment below
    // for the investigation that's still open. This carry-remainder
    // accumulator (the same pattern invaders_machine.cpp's frame/cycle
    // carryover already uses) still stands on its own merits: no division
    // anywhere in this function, compile-time-constant or otherwise.

    uint32_t isr_t0 = timer_hw->timerawl; // DEBUG -- see g_isr_time_us_accum's doc comment above for why NOT time_us_64()
    uint32_t active_now = 0;              // DEBUG: counted once per call, not per sample -- channels[] doesn't change composition mid-call
    for (int c = 0; c < MAX_CHANNELS; c++) if (channels[c].active) active_now++;
    if (active_now > g_isr_max_active_channels) g_isr_max_active_channels = active_now;

    for (int i = 0; i < count; i++) {
        uint64_t target_cycle = g_target_cycle;
        bool speaker = speaker_level_at(target_cycle);

        g_target_cycle += LRESCUE_CYCLES_PER_SAMPLE_INT;
        g_target_cycle_frac += LRESCUE_CYCLES_PER_SAMPLE_FRAC;
        if (g_target_cycle_frac >= LRESCUE_AUDIO_SAMPLE_RATE) {
            g_target_cycle_frac -= LRESCUE_AUDIO_SAMPLE_RATE;
            g_target_cycle += 1;
        }
        // Bit-banged square wave: hold whatever level was in effect at this
        // instant. A steady (non-toggling) level contributes a constant DC
        // bias rather than silence -- that matches how the real 1-bit
        // speaker hardware (and MAME's own SPEAKER_SOUND device) behaves
        // too; only the *transitions* are audible once downstream AC
        // coupling removes the constant offset. If the Fruit Jam's DAC/I2S
        // path doesn't AC-couple this cleanly in practice, this is the
        // place to add a DC-blocking highpass on this contribution.
        //
        // DEBUG: SPEAKER_DEBUG_SILENT forces this channel's audible
        // contribution to 0 while leaving every surrounding computation
        // (the event-queue drain above, decimation, everything else in
        // this loop) running exactly as normal -- isolates "does the
        // actual output signal matter" from "does the code path's cost
        // matter", since 37 calls/frame already ruled the latter out.
#define SPEAKER_DEBUG_SILENT 0
#if SPEAKER_DEBUG_SILENT
        int32_t mix = 0; (void)speaker;
#else
        int32_t mix = speaker ? SPEAKER_AMPLITUDE : -SPEAKER_AMPLITUDE;
#endif
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
        if (g_muted) mix = 0;
        if (mix >  32767) mix =  32767;
        if (mix < -32768) mix = -32768;
        int16_t s = (int16_t)mix;
        buf[i] = ((int32_t)s << 16) | (uint16_t)s;
    }

    uint32_t isr_dur = timer_hw->timerawl - isr_t0; // DEBUG -- unsigned subtraction, wrap-safe
    g_isr_time_us_accum += isr_dur;
    if (isr_dur > g_isr_max_single_call_us) g_isr_max_single_call_us = isr_dur;
    g_isr_invocation_count++;
}

void lrescue_audio_debug_isr_stats(uint32_t *isr_us_accum, uint32_t *max_single_call_us,
                                    uint32_t *max_active_channels, uint32_t *invocation_count) {
    uint32_t saved = hal_audio_enter_critical();
    if (isr_us_accum)        *isr_us_accum        = g_isr_time_us_accum;
    if (max_single_call_us)  *max_single_call_us  = g_isr_max_single_call_us;
    if (max_active_channels) *max_active_channels = g_isr_max_active_channels;
    if (invocation_count)    *invocation_count    = g_isr_invocation_count;
    g_isr_time_us_accum = 0;
    g_isr_max_single_call_us = 0;
    g_isr_max_active_channels = 0;
    g_isr_invocation_count = 0;
    hal_audio_exit_critical(saved);
}

static int g_loaded_count = 0;
static lrescue_audio_status_t g_status[LRESCUE_NUM_SAMPLES];

int lrescue_audio_load_samples(void) {
    memset(channels, 0, sizeof(channels));
    uint32_t ram_off = 0;
    int loaded = 0;

    for (int i = 0; i < LRESCUE_NUM_SAMPLES; i++) {
        g_status[i] = LRESCUE_AUDIO_STATUS_NOT_ATTEMPTED;
        char path[48];
        snprintf(path, sizeof(path), "/samples/%s.wav", SAMPLE_FILENAMES[i]);

        hal_file_t *f = hal_storage_open(path);
        if (!f) {
            Serial.printf("%-20s OPEN FAILED\n", SAMPLE_FILENAMES[i]);
            g_status[i] = LRESCUE_AUDIO_STATUS_OPEN_FAILED;
            continue;
        }

        uint32_t header_bytes = hal_storage_read(f, header_buf, WAV_HEADER_BUF_SIZE);
        wav_fmt_t fmt;
        if (header_bytes == 0 || !parse_wav_header(header_buf, header_bytes, &fmt)) {
            Serial.printf("%-20s HEADER FAILED (header_bytes=%lu)\n", SAMPLE_FILENAMES[i], (unsigned long)header_bytes);
            g_status[i] = LRESCUE_AUDIO_STATUS_HEADER_FAILED;
            hal_storage_close(f);
            continue;
        }

        uint32_t start_off = ram_off;
        uint32_t written = stream_convert_to_pcm_ram(f, &fmt, &ram_off);
        hal_storage_close(f);

        Serial.printf("%-20s data_bytes=%-7lu written=%-6lu ram_off_after=%lu\n",
                       SAMPLE_FILENAMES[i], (unsigned long)fmt.data_bytes,
                       (unsigned long)written, (unsigned long)ram_off);

        if (written == 0) {
            g_status[i] = LRESCUE_AUDIO_STATUS_NO_DATA_WRITTEN;
            continue;
        }

        wav_info[i].pcm      = pcm_ram + start_off;
        wav_info[i].bytes    = written;
        wav_info[i].step     = 0x10000; // already resampled to LRESCUE_AUDIO_SAMPLE_RATE
        wav_info[i].is_16bit = false;    // already converted to 8-bit unsigned
        wav_info[i].valid    = true;
        g_status[i] = LRESCUE_AUDIO_STATUS_OK;
        loaded++;
    }
    Serial.printf("lrescue_audio_load_samples: %d/%d loaded\n", loaded, LRESCUE_NUM_SAMPLES);

    // See lrescue_audio_now_cycles()'s doc comment: this epoch must be
    // captured as close as possible to the moment g_target_cycle starts
    // counting from 0 (the audio ISR's first invocation, which follows
    // shortly after the callback below is registered) for the two clocks to
    // agree on "now" from the start.
    g_audio_epoch_us = time_us_64();
    hal_audio_set_fill_callback(fill_audio_buffer);
    g_loaded_count = loaded;
    return loaded;
}

int lrescue_audio_loaded_count(void) {
    return g_loaded_count;
}

bool lrescue_audio_sample_valid(lrescue_sample_t sample) {
    if (sample < 0 || sample >= LRESCUE_NUM_SAMPLES) return false;
    return wav_info[sample].valid;
}

lrescue_audio_status_t lrescue_audio_sample_status(lrescue_sample_t sample) {
    if (sample < 0 || sample >= LRESCUE_NUM_SAMPLES) return LRESCUE_AUDIO_STATUS_NOT_ATTEMPTED;
    return g_status[sample];
}

uint32_t lrescue_audio_sample_bytes(lrescue_sample_t sample) {
    if (sample < 0 || sample >= LRESCUE_NUM_SAMPLES) return 0;
    return wav_info[sample].valid ? wav_info[sample].bytes : 0;
}

void lrescue_audio_play(lrescue_sample_t sample) {
    if (sample < 0 || sample >= LRESCUE_NUM_SAMPLES) return;
    if (!wav_info[sample].valid) return;

    // Find a free channel within the concurrency cap; if all of those are
    // busy, reuse the one furthest along -- see MAX_CONCURRENT_CHANNELS'
    // doc comment for why this is capped below MAX_CHANNELS (the array's
    // own size) rather than searching the full array.
    int slot = 0;
    uint32_t max_pos = 0;
    bool found = false;
    for (int c = 0; c < MAX_CONCURRENT_CHANNELS && !found; c++) {
        if (!channels[c].active) { slot = c; found = true; break; }
        if (channels[c].pos > max_pos) { max_pos = channels[c].pos; slot = c; }
    }

    sound_channel_t next = {};
    next.pcm       = wav_info[sample].pcm;
    next.pcm_bytes = wav_info[sample].bytes;
    next.step      = wav_info[sample].step;
    next.is_16bit  = wav_info[sample].is_16bit;
    next.pos       = 0;
    next.loop      = (sample == LRESCUE_SND_SHOOTINGSTAR); // loops until explicitly stopped
    next.active    = true;

    uint32_t saved = hal_audio_enter_critical();
    channels[slot] = next;
    hal_audio_exit_critical(saved);
}

void lrescue_audio_stop(lrescue_sample_t sample) {
    if (sample < 0 || sample >= LRESCUE_NUM_SAMPLES) return;
    if (!wav_info[sample].valid) return;
    uint32_t saved = hal_audio_enter_critical();
    for (int c = 0; c < MAX_CHANNELS; c++) {
        if (channels[c].active && channels[c].pcm == wav_info[sample].pcm)
            channels[c].active = false;
    }
    hal_audio_exit_critical(saved);
}

void lrescue_audio_set_mute(bool muted) {
    g_muted = muted;
}

static uint64_t g_speaker_last_pushed_cycle = 0;
static bool     g_speaker_has_pushed = false;

void lrescue_audio_speaker_event(uint64_t cycle, bool level) {
    uint32_t saved = hal_audio_enter_critical();

    // Decimate: if the most recently pushed event is still unconsumed
    // (queue non-empty) and within LRESCUE_CYCLES_PER_SAMPLE_INT cycles of
    // this one, overwrite it in place instead of growing the queue. This
    // is lossless for playback -- the consumer only ever resolves "the
    // most recent event at or before target_cycle", and two transitions
    // closer together than one output sample's cycle width already
    // collapse to the later one's level once drained, since that's finer
    // time resolution than this channel can represent at all. What this
    // actually buys is bounding queue growth (and therefore the consumer's
    // per-call drain cost, see speaker_level_at()'s DRAIN_LIMIT) for any
    // burst of writes denser than one per output sample -- which a
    // fast/high-pitched passage can produce plenty of, all within one
    // video frame's real-time burst of CPU execution.
    if (g_speaker_has_pushed && speaker_events_head != speaker_events_tail &&
        cycle - g_speaker_last_pushed_cycle < LRESCUE_CYCLES_PER_SAMPLE_INT) {
        uint32_t last_idx = (speaker_events_head + SPEAKER_EVENT_QUEUE_SIZE - 1) % SPEAKER_EVENT_QUEUE_SIZE;
        speaker_events[last_idx].cycle = cycle;
        speaker_events[last_idx].level = level;
    } else {
        uint32_t next_head = (speaker_events_head + 1) % SPEAKER_EVENT_QUEUE_SIZE;
        if (next_head != speaker_events_tail) { // room available
            speaker_events[speaker_events_head].cycle = cycle;
            speaker_events[speaker_events_head].level = level;
            speaker_events_head = next_head;
            g_speaker_events_pushed_total++;
            uint32_t depth = (next_head + SPEAKER_EVENT_QUEUE_SIZE - speaker_events_tail) % SPEAKER_EVENT_QUEUE_SIZE;
            if (depth > g_speaker_queue_peak_depth) g_speaker_queue_peak_depth = depth;
        } else {
            // queue full -- drop this event rather than overwrite an
            // unread one or block Core 0. See this array's doc comment for
            // why this shouldn't happen in practice.
            g_speaker_events_dropped_total++;
        }
    }
    g_speaker_last_pushed_cycle = cycle;
    g_speaker_has_pushed = true;

    hal_audio_exit_critical(saved);
}

void lrescue_audio_speaker_debug_stats(uint32_t *pushed, uint32_t *dropped,
                                        uint32_t *peak_depth, uint32_t *drain_limit_hits) {
    if (pushed)          *pushed          = g_speaker_events_pushed_total;
    if (dropped)         *dropped         = g_speaker_events_dropped_total;
    if (peak_depth)      *peak_depth      = g_speaker_queue_peak_depth;
    if (drain_limit_hits) *drain_limit_hits = g_speaker_drain_limit_hits;
}
