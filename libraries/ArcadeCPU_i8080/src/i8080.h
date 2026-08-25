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
    uint8_t memory[0x4000];  // 8K ROM + 8K RAM
    Condition_codes cc;
    uint8_t int_enable;
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
