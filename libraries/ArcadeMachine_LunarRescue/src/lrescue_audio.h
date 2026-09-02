// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Lunar Rescue sample-based sound mixer + one synthesized channel --
// board-agnostic. Structurally a sibling of ArcadeMachine_Invaders'
// invaders_audio.*, but with two differences driven by what MAME's own
// midw8080/8080bw_a.cpp actually does for this game:
//
//  1. MAME's lrescue_sample_names[] are descriptive filenames
//     (alienexplosion, thrust, ...), not the bare numbered files (0.wav..
//     9.wav) Invaders' MAME sample set uses -- so this loader reads
//     /samples/<name>.wav directly instead of translating through a
//     game-slot -> MAME-file-number table. This also means a stock MAME
//     "lrescue" sample pack's filenames can be dropped in with no renaming.
//     Whatever format that file is in (this game's real captured samples
//     are 44.1kHz/16-bit -- ~895KB total, nowhere close to fitting in SRAM
//     as-is) gets downsampled to 8-bit mono at LRESCUE_AUDIO_SAMPLE_RATE
//     while streaming in from storage; see lrescue_audio.cpp for the exact
//     budget this was sized against. That's a real, audible quality
//     reduction versus the source files, not a bug.
//  2. Lunar Rescue has one genuinely synthesized sound: port 5 bit 3 drives
//     a bare 1-bit speaker (MAME: SPEAKER_SOUND device) for two "bitstream"
//     jingles (end-of-level, bonus1) -- the original CPU shapes the whole
//     waveform itself by toggling that bit at audio rate. There's no WAV
//     for this; lrescue_audio_speaker_event() feeds a real bit-banged
//     square-wave channel in the mixer, reconstructed from cycle-
//     timestamped port writes rather than polled live (see that function's
//     doc comment for why polling doesn't work here) -- this is the first
//     genuinely-synthesized (not sampled) channel in this codebase, and the
//     cycle-timestamp pattern it uses is written to be liftable into a
//     shared location once a second game needs the same thing.
#ifndef LRESCUE_AUDIO_H
#define LRESCUE_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Game-semantic sample slots, in MAME's own lrescue_sample_names[] order
// (midw8080/8080bw_a.cpp) -- each loads from /samples/<name-below>.wav.
typedef enum {
    LRESCUE_SND_ALIEN_EXPLOSION = 0,  // /samples/alienexplosion.wav
    LRESCUE_SND_RESCUESHIP_EXPLOSION, // /samples/rescueshipexplosion.wav -- player death (you fly the rescue ship)
    LRESCUE_SND_BEAMGUN,              // /samples/beamgun.wav             -- player shot
    LRESCUE_SND_THRUST,               // /samples/thrust.wav
    LRESCUE_SND_BONUS2,               // /samples/bonus2.wav              -- bonus, counting rescued men
    LRESCUE_SND_BONUS3,               // /samples/bonus3.wav              -- bonus ship (MAME comment: "not confirmed")
    LRESCUE_SND_SHOOTINGSTAR,         // /samples/shootingstar.wav        -- loops; also used for rescue-ship-present
    LRESCUE_SND_STEPL,                // /samples/stepl.wav               -- footstep low tone
    LRESCUE_SND_STEPH,                // /samples/steph.wav               -- footstep high tone
    LRESCUE_NUM_SAMPLES
} lrescue_sample_t;

#define LRESCUE_AUDIO_SAMPLE_RATE 22050 // Hz, output rate fed to hal_audio_init()

// Loads /samples/<name>.wav for each slot above via ArcadeHAL storage, and
// registers the mixer with hal_audio_set_fill_callback(). Call after
// hal_storage_mount() and hal_audio_init(LRESCUE_AUDIO_SAMPLE_RATE). A
// missing individual sample file is non-fatal; returns the count
// (0..LRESCUE_NUM_SAMPLES) that DID load, so the caller can treat "0
// loaded" as an asset-load failure.
int lrescue_audio_load_samples(void);

// DEBUG: fine-grained per-slot load result, recorded by
// lrescue_audio_load_samples() -- for a sketch to report exactly where
// loading failed (not just that it did) without a serial console (this
// board's USB is a PIO host port, not a debug link -- see CLAUDE.md; see
// lrescue_fruitjam.ino's self-test). Safe to delete once sound loading is
// confirmed working end to end.
typedef enum {
    LRESCUE_AUDIO_STATUS_NOT_ATTEMPTED = 0,
    LRESCUE_AUDIO_STATUS_OK,
    LRESCUE_AUDIO_STATUS_OPEN_FAILED,      // hal_storage_open() returned NULL
    LRESCUE_AUDIO_STATUS_HEADER_FAILED,    // opened, but header read/parse failed
    LRESCUE_AUDIO_STATUS_NO_DATA_WRITTEN,  // header parsed fine, but 0 bytes came out the other end
} lrescue_audio_status_t;

int lrescue_audio_loaded_count(void);
bool lrescue_audio_sample_valid(lrescue_sample_t sample);
lrescue_audio_status_t lrescue_audio_sample_status(lrescue_sample_t sample);

// DEBUG: the converted byte count lrescue_audio_load_samples() recorded for
// a loaded slot (0 if it didn't load). At LRESCUE_AUDIO_SAMPLE_RATE, 8-bit
// mono, this is also the playback duration in samples -- e.g. 38724 bytes
// is ~1.76 real seconds. For checking whether a slot's conversion ran away
// instead of stopping at its file's true length.
uint32_t lrescue_audio_sample_bytes(lrescue_sample_t sample);

// Starts/stops game sound slot `sample`. Safe to call from the i8080
// port-write path; internally interrupt-safe against the audio ISR.
void lrescue_audio_play(lrescue_sample_t sample);
void lrescue_audio_stop(lrescue_sample_t sample);

// Global sound mute (port 3 bit 5 on real hardware -- MAME calls this
// machine().sound().system_mute()). Silences the mix without touching
// channel state, so playback picks back up correctly when un-muted.
void lrescue_audio_set_mute(bool muted);

// Records a speaker-level transition at the moment it happens, for the
// mixer to reconstruct later -- call this from lrescue_ports.cpp's port 5
// write handler with `cycle` = lrescue_audio_now_cycles() (NOT system->
// total_cycles -- see that function's doc comment for why) at the instant
// of that write, and `level` = the new bit-3 state.
//
// This does NOT just set "the current level" the way an earlier version of
// this function did. That polling approach cannot work for this specific
// channel: a whole video frame's worth of i8080 instructions (and thus
// port writes) execute in a real-time burst on this hardware -- our
// interpreter runs far faster than the original 1.9968MHz chip did, and
// frame pacing comes entirely from blocking on the video queue between
// frames, not from pacing individual instructions (see
// invaders_pico's/this project's own notes on why that's deliberate). If
// the mixer just polled "what's the level right now" once per audio tick,
// an entire musical phrase's worth of transitions -- which the original
// hardware would have spread smoothly across a real 16.67ms frame --
// would appear to happen almost instantly, then freeze, because our CPU
// races through them before the audio ISR gets more than one or two
// chances to look.
//
// The fix is to timestamp each transition in CYCLE units (a logical clock
// that's well-defined regardless of how fast our own hardware executes
// it) and have the mixer -- which runs at a real, hardware-paced rate --
// convert its own sample position into an equivalent cycle count via
// LRESCUE_CPU_HZ, then ask "what was the level at that cycle?" using the
// full history of transitions recorded during whatever frame(s) already
// executed. Because a frame's writes are always fully recorded before its
// worth of real time elapses (the CPU races ahead, it never falls behind),
// this reconstruction is always working from complete information by the
// time it's needed.
void lrescue_audio_speaker_event(uint64_t cycle, bool level);

// Real-time-derived equivalent of "what cycle is it right now" -- a real
// hardware timer, epoch-aligned to the same moment the audio ISR's own
// clock (g_target_cycle in lrescue_audio.cpp) starts counting (see
// lrescue_audio_load_samples()).
//
// CAUTION, learned the hard way: this is the WRONG clock for timestamping
// speaker events in a frame-batched CPU emulator like lrescue_machine.cpp's
// lrescue_run_frame() -- do not repeat that mistake. Core 0 races through
// an entire video frame's worth of i8080 instructions in a couple
// milliseconds of real time, then blocks on the video queue for the rest
// of that frame's real ~16.7ms. system->total_cycles increments smoothly
// across that brief real-time burst, spreading each write's *logical*
// position evenly across the frame's cycle budget -- which is exactly what
// lets the audio ISR's real-time-paced g_target_cycle stretch those events
// back out correctly when it reconstructs them. Timestamping with THIS
// function instead collapses a whole frame's writes to nearly the same
// real instant, undoing that stretch and reproducing the "phrase happens
// all at once" failure the whole cycle-timestamp scheme exists to prevent
// (heard as a buzz-then-silence pattern, once per frame). For that
// producer, use system->total_cycles, and if it's drifting from real time,
// fix the FRAMERATE assumption that drift traces back to (see
// lrescue_machine.cpp) -- don't change which clock domain the timestamp
// uses.
//
// This function is still the right tool for: (1) a producer with no
// frame-batching structure at all (e.g. lrescue_speaker_isolation_test's
// synthetic event loop, which calls lrescue_audio_speaker_event() directly
// from Arduino's loop() in real time, with nothing resembling total_cycles
// to lean on), and (2) diagnostics/calibration -- see
// lrescue_audio_debug_target_cycle()'s doc comment, and FRAMERATE's, for
// how comparing this against total_cycles is what caught the 0.83%
// framerate mismatch in the first place.
uint64_t lrescue_audio_now_cycles(void);

// DEBUG: ring-buffer health counters for the speaker-event queue --
// `pushed`/`dropped` are lifetime totals (dropped = queue was full and this
// event never made it in at all -- distinct from decimation, which is
// lossless), `peak_depth` is the largest head-to-tail distance ever
// observed (out of SPEAKER_EVENT_QUEUE_SIZE), and `drain_limit_hits` counts
// how many times speaker_level_at()'s consumer-side loop used its full
// DRAIN_LIMIT iterations while events *remained* eligible to drain --  i.e.
// genuinely fell behind that call, not just "had a few queued". All
// monotonic counters; a sketch can poll this periodically (not from inside
// any hot path) to tell directly whether the reconstruction is keeping up
// in practice, instead of inferring it indirectly from what the speaker
// audibly does. Safe to delete once the red-scanline/crumbly-audio
// investigation is resolved.
void lrescue_audio_speaker_debug_stats(uint32_t *pushed, uint32_t *dropped,
                                        uint32_t *peak_depth, uint32_t *drain_limit_hits);

// DEBUG: the audio ISR's own notion of "current cycle" (the same cycle axis
// lrescue_audio_speaker_event() expects). Compare this against whatever
// cycle value a caller is about to timestamp a new event with (system->
// total_cycles, or an isolation test's own fake-cycle clock) to check
// directly whether the two are in sync, rather than inferring it from what
// the speaker audibly does. Safe to delete once the red-scanline/crumbly-
// audio investigation is resolved.
uint64_t lrescue_audio_debug_target_cycle(void);

// DEBUG: the audio ISR's (fill_audio_buffer's) own cost -- both a total
// (isr_us_accum) and, separately, the single WORST individual invocation
// (max_single_call_us) -- plus the peak number of channels found
// concurrently active in any single invocation. All accumulated SINCE THE
// LAST CALL to this function (it resets what it reads, under the critical
// section, so it can be polled at a fixed cadence -- e.g. once/sec from
// lrescue_run_frame() -- for a rate rather than a lifetime total). Why this
// matters: this ISR runs on Core 0 and can preempt whatever else Core 0 is
// doing (the CPU-emulation loop, render_scanline(), or the blocking
// hal_video_acquire_scanline()/hal_video_submit_scanline() calls) at any
// point, for however long that particular invocation takes -- and that
// cost scales with how many channels are simultaneously active (the per-
// sample mixing loop). A wall-clock-only measurement on the caller side
// (e.g. lrescue_run_frame()'s own render_us/block_us split) can't
// distinguish "genuinely waiting on Core 1" from "briefly interrupted by my
// own audio ISR running longer because more sounds are playing at once" --
// this is the direct way to check that. isr_us_accum alone (a ~1s average)
// can dilute and hide a single rare slow call (e.g. a channel hitting a
// loop-restart edge case) -- max_single_call_us is what catches that
// specifically: if it spikes around a red-line report while isr_us_accum's
// average doesn't move much, that's a single-invocation outlier, not a
// sustained load increase. invocation_count is just useful context (how
// many times this ISR ran in the reporting window) -- not itself
// diagnostic. Safe to delete once the red-scanline investigation is
// resolved.
void lrescue_audio_debug_isr_stats(uint32_t *isr_us_accum, uint32_t *max_single_call_us,
                                    uint32_t *max_active_channels, uint32_t *invocation_count);

#ifdef __cplusplus
}
#endif

#endif
