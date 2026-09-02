// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Intel MCS-48 interpreter -- see mcs48.h for the architecture notes.
//
// Opcode semantics, machine-cycle counts, the timer/counter prescaler and
// the interrupt model are transcribed from MAME's mcs48_cpu_device
// (src/devices/cpu/mcs48/mcs48.cpp, upstream mamedev/mame). Where this file
// deviates it says so and why; there are only two places, both listed here:
//
//  - The 8243 port expander (MOVD/ANLD/ORLD P4-P7) is not implemented. No
//    machine in this project has one wired, and a silent no-op would be
//    worse than an explicit one, so those opcodes are handled as documented
//    no-ops that still burn the right cycles.
//  - The UPI-41 variants (DBB, STS, OBF/IBF flags, DMA) are not
//    implemented. Those opcodes do not exist on an 8035/MB8884; they decode
//    here as illegal, which is what the real part does.
#include "mcs48.h"

// This interpreter runs ~6,600 machine cycles per video frame to keep Donkey
// Kong's audio FIFO fed, inside a frame budget the video renderer has
// already spent most of. Left in flash it costs enough XIP time to starve
// the DVI scanline queue outright -- measured: `work` 9.5ms -> 14.5ms of a
// 16.66ms budget the moment sound was switched on, with frame pacing
// visibly breaking up. Same lever, same reason, as ArcadeCPU_Z80.
//
// `.time_critical*` is collected into RAM by the arduino-pico linker
// scripts, the same mechanism pico-sdk's __not_in_flash_func() uses;
// spelled out as a raw section attribute so this stays a portable C file
// with no pico-sdk include (the host harnesses compile it natively).
#if defined(ARDUINO_ARCH_RP2040) || defined(PICO_ON_DEVICE)
#define MCS48_RAMFUNC __attribute__((section(".time_critical.mcs48")))
#else
#define MCS48_RAMFUNC
#endif

#define C_FLAG 0x80
#define A_FLAG 0x40
#define F_FLAG 0x20
#define B_FLAG 0x10

// R0-R7 live in RAM, at 0 or 24 depending on PSW bit 4 -- see mcs48.h.
#define R(n) (cpu->ram[(uint8_t)((cpu->regptr + (n)) & cpu->ram_mask)])
#define R0 R(0)
#define R1 R(1)

MCS48_RAMFUNC static inline uint8_t ram_r(mcs48 *cpu, uint8_t addr) {
    return cpu->ram[addr & cpu->ram_mask];
}
MCS48_RAMFUNC static inline void ram_w(mcs48 *cpu, uint8_t addr, uint8_t data) {
    cpu->ram[addr & cpu->ram_mask] = data;
}

MCS48_RAMFUNC static inline void update_regptr(mcs48 *cpu) {
    cpu->regptr = (cpu->psw & B_FLAG) ? 24 : 0;
}

// The PC increments within an 2KB page, preserving the bank bit. This is
// the single most surprising thing about this architecture -- see mcs48.h
// note 1.
MCS48_RAMFUNC static inline uint8_t fetch(mcs48 *cpu) {
    uint16_t address = cpu->pc;
    cpu->pc = (uint16_t)(((cpu->pc + 1) & 0x7FF) | (cpu->pc & 0x800));
    return cpu->program_r(cpu, address);
}

MCS48_RAMFUNC static inline uint8_t test_line(mcs48 *cpu, uint8_t line) {
    return cpu->test_r ? cpu->test_r(cpu, line) : 1;
}

MCS48_RAMFUNC static inline void port_write(mcs48 *cpu, uint8_t port, uint8_t data) {
    if (cpu->port_w) cpu->port_w(cpu, port, data);
}
MCS48_RAMFUNC static inline uint8_t port_read(mcs48 *cpu, uint8_t port) {
    return cpu->port_r ? cpu->port_r(cpu, port) : 0xFF;
}

// --- timer ---------------------------------------------------------------

// Transcribed from MAME's burn_cycles(). The prescaler divides machine
// cycles by 32 in timer mode; in counter mode T1 is sampled once per cycle
// and a 1->0 transition (history bits == 0b10) clocks the counter.
MCS48_RAMFUNC static void burn_cycles(mcs48 *cpu, int count) {
    if (cpu->timecount_enabled) {
        bool timerover = false;

        if (cpu->timecount_enabled & MCS48_TIMER_ENABLED) {
            uint8_t oldtimer = cpu->timer;
            cpu->prescaler = (uint8_t)(cpu->prescaler + count);
            cpu->timer = (uint8_t)(cpu->timer + (cpu->prescaler >> 5));
            cpu->prescaler &= 0x1F;
            timerover = cpu->timer < oldtimer;
        } else if (cpu->timecount_enabled & MCS48_COUNTER_ENABLED) {
            for (; count > 0; count--) {
                cpu->t1_history = (uint8_t)((cpu->t1_history << 1) | (test_line(cpu, 1) & 1));
                if ((cpu->t1_history & 3) == 2) {
                    if (++cpu->timer == 0) timerover = true;
                }
            }
        }

        if (timerover) {
            cpu->timer_flag = true;
            // Per the docs: an overflow with the timer interrupt disabled
            // is NOT latched.
            if (cpu->tirq_enabled) cpu->timer_overflow = true;
        }
    }
}

// --- helpers -------------------------------------------------------------

MCS48_RAMFUNC static void push_pc_psw(mcs48 *cpu) {
    uint8_t sp = cpu->psw & 0x07;
    ram_w(cpu, (uint8_t)(8 + 2 * sp), (uint8_t)cpu->pc);
    ram_w(cpu, (uint8_t)(9 + 2 * sp), (uint8_t)(((cpu->pc >> 8) & 0x0F) | (cpu->psw & 0xF0)));
    cpu->psw = (uint8_t)((cpu->psw & 0xF0) | ((sp + 1) & 0x07));
}

MCS48_RAMFUNC static void pull_pc_psw(mcs48 *cpu) {
    uint8_t sp = (uint8_t)((cpu->psw - 1) & 0x07);
    cpu->pc  = ram_r(cpu, (uint8_t)(8 + 2 * sp));
    cpu->pc |= (uint16_t)(ram_r(cpu, (uint8_t)(9 + 2 * sp)) << 8);
    cpu->psw = (uint8_t)(((cpu->pc >> 8) & 0xF0) | sp);
    cpu->pc &= cpu->irq_in_progress ? 0x7FF : 0xFFF;
    update_regptr(cpu);
}

MCS48_RAMFUNC static void pull_pc(mcs48 *cpu) {
    uint8_t sp = (uint8_t)((cpu->psw - 1) & 0x07);
    cpu->pc  = ram_r(cpu, (uint8_t)(8 + 2 * sp));
    cpu->pc |= (uint16_t)(ram_r(cpu, (uint8_t)(9 + 2 * sp)) << 8);
    cpu->pc &= cpu->irq_in_progress ? 0x7FF : 0xFFF;
    cpu->psw = (uint8_t)((cpu->psw & 0xF0) | sp);
}

MCS48_RAMFUNC static void execute_add(mcs48 *cpu, uint8_t dat) {
    uint16_t temp  = (uint16_t)(cpu->a + dat);
    uint16_t temp4 = (uint16_t)((cpu->a & 0x0F) + (dat & 0x0F));
    cpu->psw &= (uint8_t)~(C_FLAG | A_FLAG);
    cpu->psw |= (uint8_t)((temp4 << 2) & A_FLAG);
    cpu->psw |= (uint8_t)((temp >> 1) & C_FLAG);
    cpu->a = (uint8_t)temp;
}

MCS48_RAMFUNC static void execute_addc(mcs48 *cpu, uint8_t dat) {
    uint8_t carryin = (uint8_t)((cpu->psw & C_FLAG) >> 7);
    uint16_t temp  = (uint16_t)(cpu->a + dat + carryin);
    uint16_t temp4 = (uint16_t)((cpu->a & 0x0F) + (dat & 0x0F) + carryin);
    cpu->psw &= (uint8_t)~(C_FLAG | A_FLAG);
    cpu->psw |= (uint8_t)((temp4 << 2) & A_FLAG);
    cpu->psw |= (uint8_t)((temp >> 1) & C_FLAG);
    cpu->a = (uint8_t)temp;
}

MCS48_RAMFUNC static void execute_jmp(mcs48 *cpu, uint16_t address) {
    // While servicing an interrupt the bank bit is forced to 0 -- an ISR
    // always runs in the low bank regardless of SEL MB.
    uint16_t a11 = cpu->irq_in_progress ? 0 : cpu->a11;
    cpu->pc = (uint16_t)(address | a11);
}

MCS48_RAMFUNC static void execute_call(mcs48 *cpu, uint16_t address) {
    push_pc_psw(cpu);
    execute_jmp(cpu, address);
}

// A conditional jump ALWAYS consumes its operand byte, taken or not.
MCS48_RAMFUNC static void execute_jcc(mcs48 *cpu, bool result) {
    uint16_t pch = cpu->pc & 0xF00;
    uint8_t offset = fetch(cpu);
    if (result) cpu->pc = (uint16_t)(pch | offset);
}

MCS48_RAMFUNC static void check_irqs(mcs48 *cpu) {
    if (cpu->irq_in_progress) return;

    if (cpu->irq_state && cpu->xirq_enabled) {
        burn_cycles(cpu, 2);
        cpu->cyc += 2;
        cpu->irq_in_progress = true;

        // MAME calls this a hack, and it is faithfully reproduced: a JNI
        // that polled and found no interrupt, immediately followed by one
        // arriving, is forced to be taken.
        if (cpu->irq_polled) {
            cpu->pc = (uint16_t)(((cpu->pc + 1) & 0x7FF) | (cpu->pc & 0x800));
            execute_jcc(cpu, true);
        }
        execute_call(cpu, 0x03);
    } else if (cpu->timer_overflow && cpu->tirq_enabled) {
        burn_cycles(cpu, 2);
        cpu->cyc += 2;
        cpu->irq_in_progress = true;
        execute_call(cpu, 0x07);
        cpu->timer_overflow = false;
    }
}

// --- public --------------------------------------------------------------

void mcs48_reset(mcs48 *cpu) {
    cpu->pc  = 0;
    cpu->psw = (uint8_t)(cpu->psw & (C_FLAG | A_FLAG));
    update_regptr(cpu);
    cpu->f1  = false;
    cpu->a11 = 0;

    cpu->tirq_enabled = false;
    cpu->xirq_enabled = false;
    cpu->timecount_enabled = 0;
    cpu->timer_flag = false;
    cpu->timer_overflow = false;
    cpu->irq_in_progress = false;
    cpu->irq_polled = false;

    // Ports 1 and 2 reset to input mode, which on this open-drain-ish
    // hardware means all ones. Machines that read back P2 depend on this.
    cpu->p1 = 0xFF;
    cpu->p2 = 0xFF;
    port_write(cpu, 1, cpu->p1);
    port_write(cpu, 2, cpu->p2);
}

void mcs48_init(mcs48 *cpu, uint16_t ram_size) {
    // Deliberately does NOT clear the callbacks or userdata -- see mcs48.h.
    for (int i = 0; i < MCS48_RAM_SIZE; i++) cpu->ram[i] = 0;
    cpu->ram_mask = (uint8_t)((ram_size ? ram_size : 64) - 1);
    cpu->a = 0;
    cpu->psw = 0;
    cpu->timer = 0;
    cpu->prescaler = 0;
    cpu->t1_history = 0;
    cpu->irq_state = false;
    cpu->cyc = 0;
    mcs48_reset(cpu);
}

void mcs48_set_irq(mcs48 *cpu, bool state) {
    cpu->irq_state = state;
}

MCS48_RAMFUNC uint8_t mcs48_step(mcs48 *cpu) {
    uint32_t before = cpu->cyc;

    check_irqs(cpu);
    cpu->irq_polled = false;

    uint8_t op = fetch(cpu);

    // Cycle accounting: every case below sets `c` to the machine cycles the
    // instruction takes (1 or 2, per MAME's burn_cycles() calls) and the
    // timer is advanced by that amount at the end. Interrupt entry above
    // has already added its own 2.
    uint8_t c = 1;

    switch (op) {

    // ---- 0x00 ----
    case 0x00: break;                                              // NOP
    case 0x02: c = 2; if (cpu->bus_w) cpu->bus_w(cpu, cpu->a); break; // OUTL BUS,A
    case 0x03: c = 2; execute_add(cpu, fetch(cpu)); break;         // ADD A,#n
    case 0x04: c = 2; execute_jmp(cpu, (uint16_t)(fetch(cpu) | 0x000)); break;
    case 0x05: cpu->xirq_enabled = true; break;                    // EN I
    case 0x07: cpu->a--; break;                                    // DEC A
    case 0x08: c = 2; cpu->a = cpu->bus_r ? cpu->bus_r(cpu) : 0xFF; break; // INS A,BUS
    case 0x09: c = 2; cpu->a = (uint8_t)(port_read(cpu, 1) & cpu->p1); break; // IN A,P1
    case 0x0A: c = 2; cpu->a = (uint8_t)(port_read(cpu, 2) & cpu->p2); break; // IN A,P2
    case 0x0C: case 0x0D: case 0x0E: case 0x0F: c = 2; break;      // MOVD A,P4-P7 (no expander)

    // ---- 0x10 ----
    case 0x10: ram_w(cpu, R0, (uint8_t)(ram_r(cpu, R0) + 1)); break; // INC @R0
    case 0x11: ram_w(cpu, R1, (uint8_t)(ram_r(cpu, R1) + 1)); break; // INC @R1
    case 0x12: c = 2; execute_jcc(cpu, (cpu->a & 0x01) != 0); break; // JB0
    case 0x13: c = 2; execute_addc(cpu, fetch(cpu)); break;        // ADDC A,#n
    case 0x14: c = 2; execute_call(cpu, (uint16_t)(fetch(cpu) | 0x000)); break;
    case 0x15: cpu->xirq_enabled = false; break;                   // DIS I
    case 0x16: c = 2; execute_jcc(cpu, cpu->timer_flag); cpu->timer_flag = false; break; // JTF
    case 0x17: cpu->a++; break;                                    // INC A
    case 0x18: case 0x19: case 0x1A: case 0x1B:
    case 0x1C: case 0x1D: case 0x1E: case 0x1F: R(op & 7)++; break; // INC Rn

    // ---- 0x20 ----
    case 0x20: { uint8_t t = cpu->a; cpu->a = ram_r(cpu, R0); ram_w(cpu, R0, t); } break; // XCH A,@R0
    case 0x21: { uint8_t t = cpu->a; cpu->a = ram_r(cpu, R1); ram_w(cpu, R1, t); } break; // XCH A,@R1
    case 0x23: c = 2; cpu->a = fetch(cpu); break;                  // MOV A,#n
    case 0x24: c = 2; execute_jmp(cpu, (uint16_t)(fetch(cpu) | 0x100)); break;
    case 0x25: cpu->tirq_enabled = true; break;                    // EN TCNTI
    case 0x26: c = 2; execute_jcc(cpu, test_line(cpu, 0) == 0); break; // JNT0
    case 0x27: cpu->a = 0; break;                                  // CLR A
    case 0x28: case 0x29: case 0x2A: case 0x2B:
    case 0x2C: case 0x2D: case 0x2E: case 0x2F:
        { uint8_t t = cpu->a; cpu->a = R(op & 7); R(op & 7) = t; } break; // XCH A,Rn

    // ---- 0x30 ----
    case 0x30: { uint8_t o = ram_r(cpu, R0); ram_w(cpu, R0, (uint8_t)((o & 0xF0) | (cpu->a & 0x0F)));
                 cpu->a = (uint8_t)((cpu->a & 0xF0) | (o & 0x0F)); } break; // XCHD A,@R0
    case 0x31: { uint8_t o = ram_r(cpu, R1); ram_w(cpu, R1, (uint8_t)((o & 0xF0) | (cpu->a & 0x0F)));
                 cpu->a = (uint8_t)((cpu->a & 0xF0) | (o & 0x0F)); } break; // XCHD A,@R1
    case 0x32: c = 2; execute_jcc(cpu, (cpu->a & 0x02) != 0); break; // JB1
    case 0x34: c = 2; execute_call(cpu, (uint16_t)(fetch(cpu) | 0x100)); break;
    case 0x35: cpu->tirq_enabled = false; cpu->timer_overflow = false; break; // DIS TCNTI
    case 0x36: c = 2; execute_jcc(cpu, test_line(cpu, 0) != 0); break; // JT0
    case 0x37: cpu->a ^= 0xFF; break;                              // CPL A
    case 0x39: c = 2; cpu->p1 = cpu->a; port_write(cpu, 1, cpu->p1); break; // OUTL P1,A
    case 0x3A: c = 2; cpu->p2 = cpu->a; port_write(cpu, 2, cpu->p2); break; // OUTL P2,A
    case 0x3C: case 0x3D: case 0x3E: case 0x3F: c = 2; break;      // MOVD P4-P7,A (no expander)

    // ---- 0x40 ----
    case 0x40: cpu->a |= ram_r(cpu, R0); break;                    // ORL A,@R0
    case 0x41: cpu->a |= ram_r(cpu, R1); break;                    // ORL A,@R1
    case 0x42: cpu->a = cpu->timer; break;                         // MOV A,T
    case 0x43: c = 2; cpu->a |= fetch(cpu); break;                 // ORL A,#n
    case 0x44: c = 2; execute_jmp(cpu, (uint16_t)(fetch(cpu) | 0x200)); break;
    case 0x45: if (!(cpu->timecount_enabled & MCS48_COUNTER_ENABLED))
                   cpu->t1_history = test_line(cpu, 1);
               cpu->timecount_enabled = MCS48_COUNTER_ENABLED; break; // STRT CNT
    case 0x46: c = 2; execute_jcc(cpu, test_line(cpu, 1) == 0); break; // JNT1
    case 0x47: cpu->a = (uint8_t)((cpu->a << 4) | (cpu->a >> 4)); break; // SWAP A
    case 0x48: case 0x49: case 0x4A: case 0x4B:
    case 0x4C: case 0x4D: case 0x4E: case 0x4F: cpu->a |= R(op & 7); break; // ORL A,Rn

    // ---- 0x50 ----
    case 0x50: cpu->a &= ram_r(cpu, R0); break;                    // ANL A,@R0
    case 0x51: cpu->a &= ram_r(cpu, R1); break;                    // ANL A,@R1
    case 0x52: c = 2; execute_jcc(cpu, (cpu->a & 0x04) != 0); break; // JB2
    case 0x53: c = 2; cpu->a &= fetch(cpu); break;                 // ANL A,#n
    case 0x54: c = 2; execute_call(cpu, (uint16_t)(fetch(cpu) | 0x200)); break;
    case 0x55: cpu->timecount_enabled = MCS48_TIMER_ENABLED; cpu->prescaler = 0; break; // STRT T
    case 0x56: c = 2; execute_jcc(cpu, test_line(cpu, 1) != 0); break; // JT1
    case 0x57:                                                     // DA A
        if ((cpu->a & 0x0F) > 0x09 || (cpu->psw & A_FLAG)) {
            if (cpu->a > 0xF9) cpu->psw |= C_FLAG;
            cpu->a = (uint8_t)(cpu->a + 0x06);
        }
        if ((cpu->a & 0xF0) > 0x90 || (cpu->psw & C_FLAG)) {
            cpu->a = (uint8_t)(cpu->a + 0x60);
            cpu->psw |= C_FLAG;
        }
        break;
    case 0x58: case 0x59: case 0x5A: case 0x5B:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F: cpu->a &= R(op & 7); break; // ANL A,Rn

    // ---- 0x60 ----
    case 0x60: execute_add(cpu, ram_r(cpu, R0)); break;            // ADD A,@R0
    case 0x61: execute_add(cpu, ram_r(cpu, R1)); break;            // ADD A,@R1
    case 0x62: cpu->timer = cpu->a; break;                         // MOV T,A
    case 0x64: c = 2; execute_jmp(cpu, (uint16_t)(fetch(cpu) | 0x300)); break;
    case 0x65: cpu->timecount_enabled = 0; break;                  // STOP TCNT
    case 0x67: { uint8_t newc = (uint8_t)((cpu->a << 7) & C_FLAG); // RRC A
                 cpu->a = (uint8_t)((cpu->a >> 1) | (cpu->psw & C_FLAG));
                 cpu->psw = (uint8_t)((cpu->psw & ~C_FLAG) | newc); } break;
    case 0x68: case 0x69: case 0x6A: case 0x6B:
    case 0x6C: case 0x6D: case 0x6E: case 0x6F: execute_add(cpu, R(op & 7)); break; // ADD A,Rn

    // ---- 0x70 ----
    case 0x70: execute_addc(cpu, ram_r(cpu, R0)); break;           // ADDC A,@R0
    case 0x71: execute_addc(cpu, ram_r(cpu, R1)); break;           // ADDC A,@R1
    case 0x72: c = 2; execute_jcc(cpu, (cpu->a & 0x08) != 0); break; // JB3
    case 0x74: c = 2; execute_call(cpu, (uint16_t)(fetch(cpu) | 0x300)); break;
    case 0x75: break;                                              // ENT0 CLK (no T0 clock output wired)
    case 0x76: c = 2; execute_jcc(cpu, cpu->f1); break;            // JF1
    case 0x77: cpu->a = (uint8_t)((cpu->a >> 1) | (cpu->a << 7)); break; // RR A
    case 0x78: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F: execute_addc(cpu, R(op & 7)); break; // ADDC A,Rn

    // ---- 0x80 ----
    case 0x80: c = 2; cpu->a = cpu->ext_r ? cpu->ext_r(cpu, R0) : 0xFF; break; // MOVX A,@R0
    case 0x81: c = 2; cpu->a = cpu->ext_r ? cpu->ext_r(cpu, R1) : 0xFF; break; // MOVX A,@R1
    case 0x83: c = 2; pull_pc(cpu); break;                         // RET
    case 0x84: c = 2; execute_jmp(cpu, (uint16_t)(fetch(cpu) | 0x400)); break;
    case 0x85: cpu->psw &= (uint8_t)~F_FLAG; break;                // CLR F0
    case 0x86: c = 2; cpu->irq_polled = !cpu->irq_state;           // JNI
               execute_jcc(cpu, cpu->irq_state); break;
    case 0x88: c = 2; if (cpu->bus_r && cpu->bus_w)                // ORL BUS,#n
                   cpu->bus_w(cpu, (uint8_t)(cpu->bus_r(cpu) | fetch(cpu)));
               else (void)fetch(cpu);
               break;
    case 0x89: c = 2; cpu->p1 |= fetch(cpu); port_write(cpu, 1, cpu->p1); break; // ORL P1,#n
    case 0x8A: c = 2; cpu->p2 |= fetch(cpu); port_write(cpu, 2, cpu->p2); break; // ORL P2,#n
    case 0x8C: case 0x8D: case 0x8E: case 0x8F: c = 2; break;      // ORLD P4-P7,A (no expander)

    // ---- 0x90 ----
    case 0x90: c = 2; if (cpu->ext_w) cpu->ext_w(cpu, R0, cpu->a); break; // MOVX @R0,A
    case 0x91: c = 2; if (cpu->ext_w) cpu->ext_w(cpu, R1, cpu->a); break; // MOVX @R1,A
    case 0x92: c = 2; execute_jcc(cpu, (cpu->a & 0x10) != 0); break; // JB4
    case 0x93: c = 2; cpu->irq_in_progress = false; pull_pc_psw(cpu); break; // RETR
    case 0x94: c = 2; execute_call(cpu, (uint16_t)(fetch(cpu) | 0x400)); break;
    case 0x95: cpu->psw ^= F_FLAG; break;                          // CPL F0
    case 0x96: c = 2; execute_jcc(cpu, cpu->a != 0); break;        // JNZ
    case 0x97: cpu->psw &= (uint8_t)~C_FLAG; break;                // CLR C
    case 0x98: c = 2; if (cpu->bus_r && cpu->bus_w)                // ANL BUS,#n
                   cpu->bus_w(cpu, (uint8_t)(cpu->bus_r(cpu) & fetch(cpu)));
               else (void)fetch(cpu);
               break;
    case 0x99: c = 2; cpu->p1 &= fetch(cpu); port_write(cpu, 1, cpu->p1); break; // ANL P1,#n
    case 0x9A: c = 2; cpu->p2 &= fetch(cpu); port_write(cpu, 2, cpu->p2); break; // ANL P2,#n
    case 0x9C: case 0x9D: case 0x9E: case 0x9F: c = 2; break;      // ANLD P4-P7,A (no expander)

    // ---- 0xA0 ----
    case 0xA0: ram_w(cpu, R0, cpu->a); break;                      // MOV @R0,A
    case 0xA1: ram_w(cpu, R1, cpu->a); break;                      // MOV @R1,A
    case 0xA3: c = 2; cpu->a = cpu->program_r(cpu, (uint16_t)((cpu->pc & 0xF00) | cpu->a)); break; // MOVP A,@A
    case 0xA4: c = 2; execute_jmp(cpu, (uint16_t)(fetch(cpu) | 0x500)); break;
    case 0xA5: cpu->f1 = false; break;                             // CLR F1
    case 0xA7: cpu->psw ^= C_FLAG; break;                          // CPL C
    case 0xA8: case 0xA9: case 0xAA: case 0xAB:
    case 0xAC: case 0xAD: case 0xAE: case 0xAF: R(op & 7) = cpu->a; break; // MOV Rn,A

    // ---- 0xB0 ----
    case 0xB0: c = 2; ram_w(cpu, R0, fetch(cpu)); break;           // MOV @R0,#n
    case 0xB1: c = 2; ram_w(cpu, R1, fetch(cpu)); break;           // MOV @R1,#n
    case 0xB2: c = 2; execute_jcc(cpu, (cpu->a & 0x20) != 0); break; // JB5
    case 0xB3: c = 2; cpu->pc = (uint16_t)((cpu->pc & 0xF00) |     // JMPP @A
                                cpu->program_r(cpu, (uint16_t)((cpu->pc & 0xF00) | cpu->a))); break;
    case 0xB4: c = 2; execute_call(cpu, (uint16_t)(fetch(cpu) | 0x500)); break;
    case 0xB5: cpu->f1 = !cpu->f1; break;                          // CPL F1
    case 0xB6: c = 2; execute_jcc(cpu, (cpu->psw & F_FLAG) != 0); break; // JF0
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF: c = 2; R(op & 7) = fetch(cpu); break; // MOV Rn,#n

    // ---- 0xC0 ----
    case 0xC4: c = 2; execute_jmp(cpu, (uint16_t)(fetch(cpu) | 0x600)); break;
    case 0xC5: cpu->psw &= (uint8_t)~B_FLAG; update_regptr(cpu); break; // SEL RB0
    case 0xC6: c = 2; execute_jcc(cpu, cpu->a == 0); break;        // JZ
    case 0xC7: cpu->a = (uint8_t)(cpu->psw | 0x08); break;         // MOV A,PSW
    case 0xC8: case 0xC9: case 0xCA: case 0xCB:
    case 0xCC: case 0xCD: case 0xCE: case 0xCF: R(op & 7)--; break; // DEC Rn

    // ---- 0xD0 ----
    case 0xD0: cpu->a ^= ram_r(cpu, R0); break;                    // XRL A,@R0
    case 0xD1: cpu->a ^= ram_r(cpu, R1); break;                    // XRL A,@R1
    case 0xD2: c = 2; execute_jcc(cpu, (cpu->a & 0x40) != 0); break; // JB6
    case 0xD3: c = 2; cpu->a ^= fetch(cpu); break;                 // XRL A,#n
    case 0xD4: c = 2; execute_call(cpu, (uint16_t)(fetch(cpu) | 0x600)); break;
    case 0xD5: cpu->psw |= B_FLAG; update_regptr(cpu); break;      // SEL RB1
    case 0xD7: cpu->psw = (uint8_t)(cpu->a & ~0x08); update_regptr(cpu); break; // MOV PSW,A
    case 0xD8: case 0xD9: case 0xDA: case 0xDB:
    case 0xDC: case 0xDD: case 0xDE: case 0xDF: cpu->a ^= R(op & 7); break; // XRL A,Rn

    // ---- 0xE0 ----
    case 0xE3: c = 2; cpu->a = cpu->program_r(cpu, (uint16_t)(0x300 | cpu->a)); break; // MOVP3 A,@A
    case 0xE4: c = 2; execute_jmp(cpu, (uint16_t)(fetch(cpu) | 0x700)); break;
    case 0xE5: cpu->a11 = 0x000; break;                            // SEL MB0
    case 0xE6: c = 2; execute_jcc(cpu, (cpu->psw & C_FLAG) == 0); break; // JNC
    case 0xE7: cpu->a = (uint8_t)((cpu->a << 1) | (cpu->a >> 7)); break; // RL A
    case 0xE8: case 0xE9: case 0xEA: case 0xEB:
    case 0xEC: case 0xED: case 0xEE: case 0xEF:
        c = 2; execute_jcc(cpu, --R(op & 7) != 0); break;          // DJNZ Rn

    // ---- 0xF0 ----
    case 0xF0: cpu->a = ram_r(cpu, R0); break;                     // MOV A,@R0
    case 0xF1: cpu->a = ram_r(cpu, R1); break;                     // MOV A,@R1
    case 0xF2: c = 2; execute_jcc(cpu, (cpu->a & 0x80) != 0); break; // JB7
    case 0xF4: c = 2; execute_call(cpu, (uint16_t)(fetch(cpu) | 0x700)); break;
    case 0xF5: cpu->a11 = 0x800; break;                            // SEL MB1
    case 0xF6: c = 2; execute_jcc(cpu, (cpu->psw & C_FLAG) != 0); break; // JC
    case 0xF7: { uint8_t newc = (uint8_t)(cpu->a & C_FLAG);        // RLC A
                 cpu->a = (uint8_t)((cpu->a << 1) | (cpu->psw >> 7));
                 cpu->psw = (uint8_t)((cpu->psw & ~C_FLAG) | newc); } break;
    case 0xF8: case 0xF9: case 0xFA: case 0xFB:
    case 0xFC: case 0xFD: case 0xFE: case 0xFF: cpu->a = R(op & 7); break; // MOV A,Rn

    default:
        // Illegal opcode. MAME logs and burns one cycle; a real part does
        // something undefined. Treated as a 1-cycle no-op rather than a
        // halt, for the same reason ArcadeCPU_i8080 does (invaders_pico's
        // DEVNOTES.md problem #1: a panic here wedges the whole core over
        // one bad byte).
        break;
    }

    burn_cycles(cpu, c);
    cpu->cyc += c;
    return (uint8_t)(cpu->cyc - before);
}
