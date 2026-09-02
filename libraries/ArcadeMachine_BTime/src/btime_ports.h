// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Burger Time memory maps and CPU wiring -- board-agnostic.
#ifndef BTIME_PORTS_H
#define BTIME_PORTS_H

#include "btime_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

// Points both m6502 cores' read/write (and the main CPU's read_opcode)
// callbacks at this file's handlers, with `system` as userdata. Call once
// from btime_init() before either CPU runs.
void btime_ports_wire(btime_system *system);

// Resets both CPUs through their reset vectors. Separate from wiring
// because it must happen AFTER the ROM is loaded -- a 6502 reset reads its
// PC from 0xFFFC/0xFFFD, so resetting an unloaded machine starts execution
// at 0x0000. Called by btime_load_assets().
void btime_ports_reset_cpus(btime_system *system);

// Latches a coin-insert edge into the main CPU's only interrupt source.
// Called by btime_input_update() on the press edge.
void btime_ports_coin_irq(btime_system *system);

// DEBUG: writes `cmd` to the sound latch and raises the sound CPU's IRQ,
// exactly as the main CPU's write to 0x4003 would.
//
// This exists to make sound effects AUDITIONABLE IN ISOLATION. The
// alternative is navigating the chef to the right place with scripted
// joystick input and hoping the effect fires, which is slow, unreliable,
// and gives a capture with the music mixed in -- and #47's rule is that a
// timbre question needs an isolated recording. The sound board only ever
// receives one-byte commands through this latch, so injecting them
// directly reproduces any effect on demand, with nothing else playing.
void btime_debug_inject_sound_command(btime_system *system, uint8_t cmd);

// Reports and resets the three diagnostic counters this module owns; see
// btime_machine.h's btime_debug_take_counters(), which wraps this.
void btime_ports_take_counters(uint32_t *out_vblank_reads,
                               uint32_t *out_opcode_swaps,
                               uint32_t *out_mirror_reads,
                               uint32_t *out_latch_writes,
                               uint32_t *out_system_reads);

#ifdef __cplusplus
}
#endif

#endif
