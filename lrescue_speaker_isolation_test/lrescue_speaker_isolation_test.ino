// Standing regression check for ArcadeMachine_LunarRescue's synthesized
// speaker channel (lrescue_audio.h/.cpp) -- the one genuinely bit-banged
// (not sampled) audio channel in this codebase, and the trickiest piece of
// the Lunar Rescue port to get right (see ../DEVNOTES.md problems #15 and
// #17 for the full story). Sibling to audio_test_fruitjam, but instead of
// a synthetic sine tone, this drives the REAL, PRODUCTION
// ArcadeMachine_LunarRescue mixer -- same ring buffer, same decimation,
// same cycle-timestamp reconstruction -- with a synthetic, controllable
// event pattern standing in for a real CPU emulator. No SD card, no video,
// no i8080 interpreter needed, so it's a much faster way to sanity-check
// this specific mechanism than flashing the full game and triggering
// bonus1 in play.
//
// Why this can use the real library code with no SD card:
// lrescue_audio_load_samples() tries to open all 9 WAV files regardless,
// but hal_storage_open() returns NULL immediately whenever storage isn't
// mounted -- so every file "fails to load" harmlessly, while the function
// still reaches its unconditional hal_audio_set_fill_callback() call at
// the end, registering the real fill_audio_buffer() (mixer + speaker
// reconstruction) exactly as the full game does.
//
// What a clean run sounds like: a smooth-sounding descending-then-
// ascending arpeggio (the note table below, pitch-tracked directly off a
// real bonus1 recording), free of any crumbly/buzzy degradation. If a
// future change to lrescue_audio.cpp breaks this, it should be audible
// here immediately, without needing the full game or a specific in-game
// trigger.
#include <arcade_hal_audio.h>
// arduino-cli discovers libraries to link by scanning #include directives.
// lrescue_audio.h alone pulls in ArcadeCPU_i8080 too (transitively, via
// lrescue_machine.h's #include "i8080.h") -- harmless: nothing in this
// sketch ever calls exec_opcode(), so that dead code just sits unused.
#include <board_config_fruitjam.h>
#include <lrescue_audio.h>

#define SAMPLE_RATE 22050 // must match LRESCUE_AUDIO_SAMPLE_RATE

// The actual bonus1 note sequence, pitch-tracked (autocorrelation, ~0.95-0.98
// confidence throughout every plateau) from a real-hardware/MAME reference
// recording -- not a generic stand-in shape. Descending 8 notes, ascending
// back up 7, ending on a held repeat of the starting note. Frequencies and
// per-note durations both come directly off that recording's measured
// plateaus.
struct note_t { float hz; uint32_t ms; };
static const note_t NOTES[] = {
    {1066.7f,  70}, {1000.0f,  60}, { 905.7f,  60}, { 800.0f,  60}, // descending
    { 727.3f,  65}, { 685.7f,  55}, { 615.4f,  65}, { 545.5f,  65}, // ...
    { 615.4f,  60}, { 685.7f,  60}, { 727.3f,  60}, { 800.0f,  60}, // ascending
    { 905.7f,  65}, {1000.0f,  55}, {1066.7f, 195},                // ...back to start, held
};
#define NUM_NOTES (sizeof(NOTES) / sizeof(NOTES[0]))
#define REPEAT_GAP_MS 700u // silent pause between repetitions, for easy A/B by ear

// Cycle timestamps below use lrescue_audio_now_cycles(), NOT a hand-rolled
// micros()-based clock -- an earlier version of this sketch rolled its own
// and got the epoch wrong (didn't match when the audio ISR's own clock
// starts counting), which silently overflowed the speaker ring buffer
// every repetition. That finding is what motivated adding
// lrescue_audio_now_cycles() to the library itself as the correct,
// properly epoch-aligned real-time clock for a non-frame-batched producer
// like this one -- see ../DEVNOTES.md problem #15a for the full account.
// Real games (frame-batched CPU emulators) must NOT use this same function
// for their own event timestamps -- see that function's doc comment in
// lrescue_audio.h for why, and problem #15b for what actually broke when
// an earlier attempt did.

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("Speaker-channel regression check: no video, no SD card, no CPU emulator.");
    Serial.println("Registering the real ArcadeMachine_LunarRescue audio mixer...");

    hal_audio_init(SAMPLE_RATE);
    int loaded = lrescue_audio_load_samples();
    Serial.printf("samples loaded: %d/9 (expected 0 -- no SD card mounted; harmless here)\n", loaded);

    Serial.println("Playing the real bonus1 note sequence through the speaker channel,");
    Serial.println("looping with a pause between reps for easy listening -- should sound clean.");
}

void loop() {
    static uint32_t note_start_ms = 0;
    static int      note_idx = -1; // -1 == in the inter-repetition gap
    static bool     level = false;
    static uint32_t next_toggle_us = 0;

    uint32_t now_ms = millis();
    uint32_t note_ms = (note_idx >= 0) ? NOTES[note_idx].ms : REPEAT_GAP_MS;
    if (note_start_ms == 0 || now_ms - note_start_ms >= note_ms) {
        note_start_ms = now_ms;
        note_idx++;
        if (note_idx >= (int)NUM_NOTES) note_idx = -1; // finished a rep -> gap -> restart at 0 next tick
        // Deliberately no per-note Serial print here -- Serial (USB CDC)
        // blocks on this board if nothing on the host is actively reading,
        // and during an audio-only recording (no Serial Monitor open) that
        // would freeze this whole toggle loop for a moment, holding the
        // speaker at a constant level -- silent, since a steady DC level
        // makes no sound through an AC-coupled output. An earlier version
        // printed here and produced exactly that artifact: regular silent
        // gaps with no relation to the real speaker-synthesis mechanism at
        // all. Don't add one back without reading this comment again.
    }

    // Ring-buffer health + producer/consumer cycle-clock agreement,
    // printed at a fixed ~4Hz regardless of note boundaries so it can run
    // continuously with a Serial Monitor attached without itself becoming
    // a source of blocking artifacts (see the note above). `gap` should
    // stay near zero and flat throughout a healthy run; `dropped` and
    // `drain_hits` should stay at (or very near) zero. If any of these
    // drift or climb, something regressed in the ring buffer or the
    // clock-epoch alignment between this sketch and the audio ISR.
    static uint32_t last_stats_ms = 0;
    if (now_ms - last_stats_ms >= 250) {
        last_stats_ms = now_ms;
        uint32_t pushed = 0, dropped = 0, peak_depth = 0, drain_hits = 0;
        lrescue_audio_speaker_debug_stats(&pushed, &dropped, &peak_depth, &drain_hits);
        uint64_t target = lrescue_audio_debug_target_cycle();
        uint64_t mine = lrescue_audio_now_cycles();
        int64_t gap_cycles = (int64_t)mine - (int64_t)target;
        Serial.printf("[%6lums] pushed=%-6lu dropped=%-4lu peak_depth=%-4lu drain_hits=%-4lu gap=%+7lld cyc (%+.1fms)\n",
                      (unsigned long)now_ms, (unsigned long)pushed, (unsigned long)dropped,
                      (unsigned long)peak_depth, (unsigned long)drain_hits,
                      (long long)gap_cycles, (double)gap_cycles / 1996.8);
    }

    if (note_idx < 0) return; // silent gap between repetitions -- no toggling

    uint32_t now_us = micros();
    if ((int32_t)(now_us - next_toggle_us) >= 0) {
        level = !level;
        lrescue_audio_speaker_event(lrescue_audio_now_cycles(), level);
        float half_period_us = 500000.0f / NOTES[note_idx].hz;
        next_toggle_us = now_us + (uint32_t)half_period_us;
    }
}
