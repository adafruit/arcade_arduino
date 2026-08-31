// Donkey Kong sound: an MB8884 (8035-class MCS-48) sound CPU driving a DAC,
// plus an approximation of the board's discrete analog network.
//
// Verified against MAME's dkong_a.cpp -- dkong2b_audio()'s device wiring,
// dkong_tune_r()/dkong_p1_w(), the three latch8 devices, and
// DISCRETE_SOUND_START(dkong2b_discrete).
//
// WHAT THE REAL BOARD DOES
//
// There is no sound chip. There is a microcontroller and a pile of analog
// parts, and the two make completely different kinds of sound:
//
//   - The 8035 plays MUSIC AND VOICE. It reads its own program from
//     s_3i_b.bin and reads sample bytes out of s_3j_b.bin in banked
//     256-byte pages, writing them to a DAC on port 1. That is the intro
//     tune, the hammer music, the walking sound, the "how high can you get"
//     interludes.
//   - Three DISCRETE ANALOG CHANNELS make the effects: "stomp" (an LFSR
//     noise source through a counter and a diode mixer), "jump" (a 555
//     astable whose control voltage is swept by a CD4049 inverter
//     oscillator) and "walk" (the same shape, different constants). The
//     main CPU triggers these directly by writing bits to a latch at
//     0x7D00-0x7D07 -- the 8035 is not involved.
//
// WHAT THIS PORT DOES
//
// The 8035 and its DAC path are EMULATED: a real MCS-48 core
// (ArcadeCPU_MCS48) runs the real ROM, and its port-1 writes become audio
// samples. That half is faithful.
//
// The three discrete channels are APPROXIMATED, not simulated. MAME models
// them as an analog circuit -- 555 timers, RC networks, diode mixers, a
// Sallen-Key filter -- solved in floating point. Porting that solver to run
// inside an audio ISR on this hardware is not realistic, and this project
// has done the same thing once before: Galaga's 54XX explosion channel is
// this project's own synthesis tuned by ear against a recording, not a
// circuit simulation. Each approximation below names the MAME node it
// stands in for and what was kept.
//
// The Sallen-Key low-pass on the DAC output IS implemented (as a biquad),
// because it is one filter, it is cheap, and MAME's own comment gives its
// corner frequency and Q directly: f = 1916 Hz, Q = 0.74.
#ifndef DKONG_AUDIO_H
#define DKONG_AUDIO_H

#include "dkong_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

// 22050 Hz, matching every other machine in this project.
#define DKONG_AUDIO_SAMPLE_RATE 22050

// The 8035's program ROM (s_3i_b.bin, mirrored to fill 0x1000 exactly as
// ROM_START( dkong )'s ROM_RELOAD does) and its sample ROM (s_3j_b.bin,
// read in banked 256-byte pages). Filled by dkong_assets.cpp.
#define DKONG_SOUND_ROM_SIZE 0x1000
#define DKONG_TUNE_ROM_SIZE  0x0800
extern uint8_t dkong_sound_rom[DKONG_SOUND_ROM_SIZE];
extern uint8_t dkong_tune_rom[DKONG_TUNE_ROM_SIZE];

// Brings up the sound CPU and registers the ArcadeHAL audio fill callback.
// Call after dkong_load_rom() has filled the ROMs above.
void dkong_audio_init(dkong_system *system);

// Runs the sound hardware forward far enough to keep the audio FIFO fed,
// and generates the samples the ISR will play. Called once per frame from
// dkong_run_frame(), interleaved with the main CPU.
void dkong_audio_run_frame(dkong_system *system);

// One slice of a frame's audio, called from inside the scanline pump so the
// sound work is spread across the frame instead of bursting at the end.
// Running it all in one go starves the DVI queue -- see dkong_audio.cpp.
void dkong_audio_run_slice(uint32_t slice, uint32_t nslices);

// Mean microseconds per frame spent generating sound, over the last 60
// frames -- summed across the slices, so it reports the interleaved path.
uint32_t dkong_audio_debug_cost_us(void);

// --- wiring called by dkong_ports.cpp ------------------------------------

// 0x7C00 write: the sound-command latch (MAME's "ls175.3d").
void dkong_audio_command_w(uint8_t data);

// 0x7D00-0x7D07 write: the signal latch (MAME's m_dev_6h), one bit per
// address, taken from bit 0 of the data. Bits 0/1/2 and 6/7 drive the
// discrete channels; bit 3 is readable by the 8035 on P2; bits 4 and 5 are
// the 8035's T1 and T0 test inputs.
void dkong_audio_signal_w(uint8_t offset, uint8_t data);

// 0x7D80 write: asserts or clears the 8035's external interrupt.
void dkong_audio_irq_w(uint8_t data);

// IN2 bit 6 on the main CPU is the sound CPU's status: the INVERTED bit 4
// of the 8035's port-2 latch. Returns that bit already in position 0x40.
uint8_t dkong_audio_status_r(void);

// --- debug ---------------------------------------------------------------

// FIFO health and activity since the last call, then resets the counters.
// `underruns` non-zero means the ISR ran out of samples (audible as a
// click); `overruns` means the producer got ahead and dropped some.
// Port-1 (DAC) and port-2 write counts and the DAC's value range since the
// last call. If p1_writes is zero the sound CPU is running but never
// reaching its output stage; if the range is a single value it is running
// but not playing anything.
void dkong_audio_debug_take_dac(uint32_t *out_p1_writes, uint32_t *out_p2_writes,
                                uint8_t *out_dac_min, uint8_t *out_dac_max,
                                uint8_t *out_p2_value);

// Dumps the next `instructions` sound-CPU instructions through
// dkong_audio_debug_trace_line(), which a harness can define to print them.
void dkong_audio_debug_trace(long instructions);
// 256-entry histogram of every value written to the DAC.
const uint32_t *dkong_audio_debug_dac_hist(void);
// Trigger counts for the three discrete channels since the last call.
void dkong_audio_debug_take_triggers(uint32_t *walk, uint32_t *jump, uint32_t *stomp);
// What the MAIN CPU requested: command-latch writes, sound-CPU interrupt
// assertions, per-bit signal-latch writes, and which command nibbles were
// ever sent. Answers "was this sound asked for" before "was it produced".
void dkong_audio_debug_take_requests(uint32_t *cmd_writes, uint32_t *irq_asserts,
                                     uint32_t *sig_bits, uint8_t *cmds_seen);
// Duty cycle of each signal-latch bit, in audio samples high vs total.
void dkong_audio_debug_take_duty(uint32_t *bits_high, uint32_t *samples);
// Enable/disable individual channels for diagnosis: bit 0 = DAC (music),
// 1 = stomp, 2 = jump, 3 = walk. Default 0x0F.
void dkong_audio_debug_set_channels(uint8_t mask);
// Called on every discrete-channel trigger; weakly defined as a no-op.
void dkong_audio_debug_trigger_event(const char *name);
void dkong_audio_debug_trace_line(uint16_t pc, uint8_t op, uint8_t a, uint8_t dac,
                                  uint8_t p2, uint8_t psw, uint8_t timer);

void dkong_audio_debug_take_stats(uint32_t *out_underruns, uint32_t *out_overruns,
                                  uint32_t *out_peak_depth, uint32_t *out_sound_cycles);

#ifdef __cplusplus
}
#endif

#endif
