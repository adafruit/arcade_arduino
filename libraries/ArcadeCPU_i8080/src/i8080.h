// SPDX-FileCopyrightText: 2020 Ingrid Rebecca Abraham
// SPDX-FileCopyrightText: 2024 Stephan Hotto
// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#ifndef CPU_H
#define CPU_H

#include <stdint.h>
#include <stdbool.h>

enum Register {
    A,
    B,
    C,
    D,
    E,
    H,
    L,
    SP,
    M,
    PSW,
};

typedef struct {
    bool z;
    bool s;
    bool p;
    bool cy;
    bool ac;
} Condition_codes;

typedef struct {
    uint8_t regs[7];
    uint16_t sp;
    uint16_t pc;
    uint8_t memory[0x10000]; // full 16-bit address space -- individual machines
                             // only populate the ranges their own board actually
                             // decodes ROM/RAM into (Space Invaders uses 0x4000
                             // of it; other 8080bw-family boards, e.g. Lunar
                             // Rescue, use more, and not always contiguously).
    Condition_codes cc;
    uint8_t int_enable;
    // Some 8080bw-family boards incompletely decode their address bus, aliasing
    // RAM at 0x2000-0x3fff onto 0x4000-0x5fff (Space Invaders' original PCB
    // does this, and its self-test code relies on it). Other boards in the same
    // family map real ROM at 0x4000 instead (Lunar Rescue's extra two chips
    // live at 0x4000-0x4fff) -- for those, this must stay false. Set true only
    // for a machine that has confirmed it needs the alias; see read_memory()/
    // write_memory() in i8080.c.
    bool mirror_2000_at_4000;
} Cpu_state;

// i8080.c is compiled as plain C (this header is shared verbatim with the
// invaders_pico reference clone, a pure-C project). extern "C" here is only
// for C++ callers (e.g. ArcadeMachine_Invaders's .cpp files) -- without it,
// a C++ translation unit would mangle calls to these and fail to link
// against i8080.c's plain C symbols.
#ifdef __cplusplus
extern "C" {
#endif

uint8_t read_memory(Cpu_state *state, uint16_t address);
void write_memory(Cpu_state *state, uint16_t address, uint8_t value);
int interrupt(Cpu_state *state, uint16_t offset);
int exec_opcode(Cpu_state *state);

#ifdef __cplusplus
}
#endif

#endif
