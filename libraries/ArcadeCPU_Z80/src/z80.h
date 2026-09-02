// SPDX-FileCopyrightText: 2019 Nicolas Allemand
// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Vendored verbatim from superzazu/z80 (MIT license, Copyright (c) 2019
// Nicolas Allemand -- see this library's LICENSE file). Only change from
// upstream: the extern "C" guard below, added from day one per
// arcade_arduino/DEVNOTES.md problem #2 ("C code shared with C++ code needs
// extern "C", unlike the all-C reference build" -- ArcadeCPU_i8080's i8080.h
// hit this as a real link failure; ArcadeCPU_Z80 is a C++-callable machine
// layer from the start, same as that one).
//
// z80.c is compiled as plain C. A C++ translation unit (ArcadeMachine_
// Pacman's .cpp files) calling z80_init()/z80_step()/z80_gen_int()/
// z80_gen_nmi() through an unwrapped header would have the C++ compiler
// mangle the call site's expected symbol names, producing "undefined
// reference" at link time against z80.c's plain C symbols.
//
// Unlike ArcadeCPU_i8080's Cpu_state (a flat register/memory struct polled
// by read_port()/write_port()/read_memory()/write_memory() extern globals
// the Machine layer defines), this upstream core wires hardware access via
// per-instance function pointers on the z80 struct itself (read_byte,
// write_byte, port_in, port_out, userdata) -- a different but equally
// "no hardware knowledge" shape, native to this particular vendored core.
#ifndef Z80_Z80_H_
#define Z80_Z80_H_

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct z80 z80;
struct z80 {
  uint8_t (*read_byte)(void*, uint16_t);
  void (*write_byte)(void*, uint16_t, uint8_t);
  uint8_t (*port_in)(z80*, uint8_t);
  void (*port_out)(z80*, uint8_t, uint8_t);
  void* userdata;

  // Divergence from upstream superzazu/z80 (the third; see z80.c's
  // Z80_RAMFUNC and this file's extern "C" guard): pinned to an explicit
  // 32-bit type rather than `unsigned long`. `unsigned long` is 32-bit on
  // the arm-none-eabi target but 64-bit on a typical host, which made the
  // host test harness (arcade_arduino/tools/galaga_host) structurally
  // unable to reproduce cycle-counter wraparound -- and a real freeze bug
  // hid in exactly that gap for a while (see galaga_machine.cpp's
  // interleave_to_target()). Machines deriving timing from this counter
  // should use uint32_t to match.
  uint32_t cyc; // cycle count (t-states)

  uint16_t pc, sp, ix, iy; // special purpose registers
  uint16_t mem_ptr; // "wz" register
  uint8_t a, b, c, d, e, h, l; // main registers
  uint8_t a_, b_, c_, d_, e_, h_, l_, f_; // alternate registers
  uint8_t i, r; // interrupt vector, memory refresh

  // flags: sign, zero, yf, half-carry, xf, parity/overflow, negative, carry
  bool sf : 1, zf : 1, yf : 1, hf : 1, xf : 1, pf : 1, nf : 1, cf : 1;

  uint8_t iff_delay;
  uint8_t interrupt_mode;
  uint8_t int_data;
  bool iff1 : 1, iff2 : 1;
  bool halted : 1;
  bool int_pending : 1, nmi_pending : 1;
};

#ifdef __cplusplus
extern "C" {
#endif

void z80_init(z80* const z);
void z80_step(z80* const z);
void z80_debug_output(z80* const z);
void z80_gen_nmi(z80* const z);
void z80_gen_int(z80* const z, uint8_t data);

#ifdef __cplusplus
}
#endif

#endif // Z80_Z80_H_
