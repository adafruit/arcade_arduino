// SPDX-FileCopyrightText: 2018 Nicolas Allemand
// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Vendored from superzazu/6502 (MIT license, Copyright (c) 2018 Nicolas
// Allemand -- see this library's LICENSE file), the same author as the
// upstream of ArcadeCPU_Z80. Upstream passes AllSuiteA,
// 6502_functional_test, 6502_decimal_test, 65C02_extended_opcodes_test and
// timingtest; see tools/m6502_test/ for this project's own run of the first
// three against this vendored copy.
//
// FOUR CHANGES FROM UPSTREAM, all of them things this project has already
// been bitten by once:
//
//  1. `extern "C"` guard (DEVNOTES.md problem #2). m6502.c compiles as plain
//     C and every caller here is C++; without this the call sites link
//     against mangled names that don't exist. ArcadeCPU_i8080 hit this as a
//     real link failure and ArcadeCPU_Z80 was born with the guard.
//  2. `cyc` is `uint32_t`, not upstream's `unsigned long` (DEVNOTES.md
//     problems #26/#27). `long` is 32-bit on the RP2350 and 64-bit on a
//     macOS host, so a host harness CANNOT reproduce a wraparound bug unless
//     the width matches the device -- and a cycle-counter wraparound was a
//     real permanent freeze in this project (#22). ArcadeCPU_Z80's z80.h
//     carries the identical change for the identical reason.
//  3. `read_opcode` -- an OPTIONAL second read callback, used only for the
//     instruction fetch in m6502_step(). NULL means "use read_byte", which
//     is what a plain 6502 wants. It exists because Burger Time's main CPU
//     is a DECO CPU-7: an encrypted 6502 whose descrambling applies to
//     opcode fetches only, so the machine layer needs to distinguish a
//     fetch from a data read. Keeping the hook here and the Data East
//     knowledge in ArcadeMachine_BTime is what preserves SAMP's rule that a
//     CPU library knows nothing about any machine.
//  4. `illegal_ops` -- a counter, incremented wherever the interpreter
//     treats an opcode as an undefined-behaviour NOP. Costs one increment on
//     a path that should never execute, and answers "is this ROM relying on
//     undocumented instructions" with a NUMBER rather than an opinion, which
//     is the lesson of DEVNOTES.md problem #32. Nothing in the emulation
//     reads it.
//
// Everything else, including the instruction set, the cycle tables, the NMOS
// `JMP ($xxFF)` page-wrap bug (correctly gated on m65c02_mode) and the
// interrupt sequence, is upstream's.
//
// NOTE ON CHIP VARIANT: this core emulates both NMOS 6502 and 65C02 and
// selects with `m65c02_mode`, which m6502_init() leaves at 0. Burger Time's
// CPU-7 is an NMOS part, so **leave it 0** -- the default is already right,
// but it is worth knowing the switch exists before wondering why a BCD or
// JMP-indirect edge case behaves the way it does.
#ifndef M6502_M6502_H_
#define M6502_M6502_H_

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct m6502 {
    uint8_t (*read_byte)(void*, uint16_t); // user function to read from memory
    void (*write_byte)(void*, uint16_t, uint8_t); // same for writing to memory

    // Optional instruction-fetch read. NULL => read_byte is used for the
    // opcode fetch too (an ordinary 6502). Set it only for a CPU whose
    // opcodes are decoded differently from its data, e.g. the DECO CPU-7.
    uint8_t (*read_opcode)(void*, uint16_t);

    void* userdata; // user custom pointer

    uint32_t cyc; // cycle count -- free running, wraps; compare ELAPSED
                  // cycles (a subtraction), never an absolute target

    uint16_t pc; // program counter
    uint8_t a, x, y, sp; // register A, X, Y and stack pointer

    // flags: carry, zero, interrupt disable, decimal mode,
    // break command, overflow, negative
    bool cf : 1, zf : 1, idf : 1, df : 1, bf : 1, vf : 1, nf : 1;

    bool page_crossed : 1; // helper flag to keep track of page crossing
    bool enable_bcd : 1; // helper flag to enable/disable BCD
    bool m65c02_mode : 1; // helper flag to enable 65C02 emulation

    bool stop : 1, wait : 1; // flags used with STP/WAI 65C02 instructions

    uint32_t illegal_ops; // diagnostic only -- see this file's header
} m6502;

void m6502_init(m6502* const c);
void m6502_step(m6502* const c);
void m6502_debug_output(m6502* const c);

// interrupts
void m6502_gen_nmi(m6502* const c);
void m6502_gen_res(m6502* const c);
void m6502_gen_irq(m6502* const c);

#ifdef __cplusplus
}
#endif

#endif // M6502_M6502_H_
