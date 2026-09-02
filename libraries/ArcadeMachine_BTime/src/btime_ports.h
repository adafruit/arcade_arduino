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
