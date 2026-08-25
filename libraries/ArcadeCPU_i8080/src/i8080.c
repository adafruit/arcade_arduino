// Intel 8080 emulation — ported from shotto42/invaders (MIT license, original by intarga/i8080e)
// Changes from original: removed <unistd.h>; replaced exit() with tight_loop_contents() panic.

#include <stdio.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "i8080.h"
#include "i8080_ports.h"

static inline void cpu_panic(void) {
    // Halt core on illegal or unimplemented opcode — blinks onboard LED if available
    while (1) tight_loop_contents();
}

uint8_t check_parity(uint8_t res, int bits) {
    int p = 0;
    for (int i = 0; i < bits; i++) {
        if ((res >> i) & 0x01) p++;
    }
    return ((p & 0x1) == 0);
}

void set_zsp(Cpu_state *state, uint8_t res) {
    state->cc.z = (res == 0x00);
    state->cc.s = ((res & 0x80) == 0x80);
    state->cc.p = check_parity(res, 8);
}

uint16_t get_m_address(Cpu_state *state, uint8_t reg1, uint8_t reg2) {
    return (state->regs[reg1] << 8) | state->regs[reg2];
}

uint16_t get_immediate_address(Cpu_state *state) {
    uint8_t byte1 = read_memory(state, state->pc + 2);
    uint8_t byte2 = read_memory(state, state->pc + 1);
    return (byte1 << 8) | byte2;
}

int STC(Cpu_state *state) { state->cc.cy = 1; return 4; }
int CMC(Cpu_state *state) { state->cc.cy = !state->cc.cy; return 4; }

int INR(Cpu_state *state, uint8_t reg) {
    int cyc = 5;
    uint8_t res;
    if (reg == M) {
        cyc = 10;
        uint16_t address = get_m_address(state, H, L);
        res = read_memory(state, address) + 1;
        write_memory(state, address, res);
    } else {
        res = ++state->regs[reg];
    }
    set_zsp(state, res);
    state->cc.ac = ((res & 0x0f) == 0x00);
    return cyc;
}

int DCR(Cpu_state *state, uint8_t reg) {
    int cyc = 5;
    uint8_t res;
    if (reg == M) {
        cyc = 10;
        uint16_t address = get_m_address(state, H, L);
        res = read_memory(state, address) - 1;
        write_memory(state, address, res);
    } else {
        res = --state->regs[reg];
    }
    set_zsp(state, res);
    state->cc.ac = !((res & 0x0f) == 0x0f);
    return cyc;
}

int CMA(Cpu_state *state) { state->regs[A] = ~state->regs[A]; return 4; }

int DAA(Cpu_state *state) {
    if (state->cc.ac || (state->regs[A] & 0x0f) > 9) {
        state->cc.ac = 1;
        state->regs[A] += 6;
    }
    if (state->cc.cy || (state->regs[A] >> 4) > 9) {
        state->cc.cy = 1;
        state->regs[A] = ((state->regs[A] & 0xf0) + (6 << 4)) + (state->regs[A] & 0x0f);
    }
    return 4;
}

int MOV(Cpu_state *state, uint8_t reg1, uint8_t reg2) {
    int cyc = 5;
    uint8_t byte;
    if (reg2 == M) {
        cyc = 7;
        uint16_t address = get_m_address(state, H, L);
        byte = read_memory(state, address);
    } else {
        byte = state->regs[reg2];
    }
    if (reg1 == M) {
        cyc = 7;
        uint16_t address = get_m_address(state, H, L);
        write_memory(state, address, byte);
    } else {
        state->regs[reg1] = byte;
    }
    return cyc;
}

int STAX(Cpu_state *state, uint8_t reg) {
    uint16_t address = get_m_address(state, reg, reg + 1);
    write_memory(state, address, state->regs[A]);
    return 7;
}

int LDAX(Cpu_state *state, uint8_t reg) {
    uint16_t address = get_m_address(state, reg, reg + 1);
    state->regs[A] = read_memory(state, address);
    return 7;
}

int ADD(Cpu_state *state, uint8_t reg) {
    int cyc = 4;
    uint8_t add1 = state->regs[A];
    uint8_t add2;
    if (reg == M) { cyc = 7; add2 = read_memory(state, get_m_address(state, H, L)); }
    else           { add2 = state->regs[reg]; }
    state->cc.ac = (((add1 & 0x0f) + (add2 & 0x0f)) & 0xf0) != 0;
    uint16_t res = add1 + add2;
    state->cc.cy = (res & 0x0f00) != 0;
    state->regs[A] = res & 0xff;
    set_zsp(state, state->regs[A]);
    return cyc;
}

int ADC(Cpu_state *state, uint8_t reg) {
    int cyc = 4;
    uint8_t add1 = state->regs[A];
    uint8_t add2;
    if (reg == M) { cyc = 7; add2 = read_memory(state, get_m_address(state, H, L)); }
    else           { add2 = state->regs[reg]; }
    state->cc.ac = (((add1 & 0x0f) + (add2 & 0x0f) + state->cc.cy) & 0xf0) != 0;
    uint16_t res = add1 + add2 + state->cc.cy;
    state->cc.cy = (res & 0x0f00) != 0;
    state->regs[A] = res & 0xff;
    set_zsp(state, state->regs[A]);
    return cyc;
}

int SUB(Cpu_state *state, uint8_t reg) {
    int cyc = 4;
    uint8_t sub1 = state->regs[A];
    uint8_t sub2;
    if (reg == M) { cyc = 7; sub2 = ~read_memory(state, get_m_address(state, H, L)); }
    else           { sub2 = ~state->regs[reg]; }
    state->cc.ac = (((sub1 & 0x0f) + (sub2 & 0x0f) + 1) & 0xf0) != 0;
    uint16_t res = sub1 + sub2 + 1;
    state->cc.cy = (res & 0x0f00) == 0;
    state->regs[A] = res & 0xff;
    set_zsp(state, state->regs[A]);
    return cyc;
}

int SBB(Cpu_state *state, uint8_t reg) {
    int cyc = 4;
    uint8_t sub1 = state->regs[A];
    uint8_t sub2;
    if (reg == M) { cyc = 7; sub2 = ~(read_memory(state, get_m_address(state, H, L)) + 1); }
    else           { sub2 = ~(state->regs[reg] + 1); }
    state->cc.ac = (((sub1 & 0x0f) + (sub2 & 0x0f) + 1) & 0xf0) != 0;
    uint16_t res = sub1 + sub2 + 1;
    state->cc.cy = (res & 0x0f00) == 0;
    state->regs[A] = res & 0xff;
    set_zsp(state, state->regs[A]);
    return cyc;
}

int ANA(Cpu_state *state, uint8_t reg) {
    int cyc = 4;
    uint8_t and;
    if (reg == M) { cyc = 7; and = read_memory(state, get_m_address(state, H, L)); }
    else           { and = state->regs[reg]; }
    state->cc.ac = ((state->regs[A] | and) & 0x08) != 0;
    state->regs[A] = state->regs[A] & and;
    state->cc.cy = 0;
    set_zsp(state, state->regs[A]);
    return cyc;
}

int XRA(Cpu_state *state, uint8_t reg) {
    int cyc = 4;
    uint8_t xor;
    if (reg == M) { cyc = 7; xor = read_memory(state, get_m_address(state, H, L)); }
    else           { xor = state->regs[reg]; }
    state->regs[A] = state->regs[A] ^ xor;
    state->cc.cy = 0;
    state->cc.ac = 0;
    set_zsp(state, state->regs[A]);
    return cyc;
}

int ORA(Cpu_state *state, uint8_t reg) {
    int cyc = 4;
    uint8_t or;
    if (reg == M) { cyc = 7; or = read_memory(state, get_m_address(state, H, L)); }
    else           { or = state->regs[reg]; }
    state->regs[A] = state->regs[A] | or;
    state->cc.cy = 0;
    state->cc.ac = 0;
    set_zsp(state, state->regs[A]);
    return cyc;
}

int CMP(Cpu_state *state, uint8_t reg) {
    int cyc = 4;
    uint8_t cmp1 = state->regs[A];
    uint8_t cmp2;
    if (reg == M) { cyc = 7; cmp2 = ~read_memory(state, get_m_address(state, H, L)); }
    else           { cmp2 = ~state->regs[reg]; }
    int16_t sub = cmp1 - ~cmp2;
    state->cc.ac = (~(cmp1 ^ sub ^ ~cmp2) & 0x10) != 0;
    uint16_t res = cmp1 + cmp2 + 1;
    state->cc.cy = (res & 0x0f00) == 0;
    set_zsp(state, res & 0xff);
    return cyc;
}

int RLC(Cpu_state *state) {
    state->cc.cy = (state->regs[A] & 0x80) != 0;
    state->regs[A] <<= 1;
    if (state->cc.cy) state->regs[A]++;
    return 4;
}

int RRC(Cpu_state *state) {
    state->cc.cy = (state->regs[A] & 0x01) != 0;
    state->regs[A] >>= 1;
    if (state->cc.cy) state->regs[A] += 0x80;
    return 4;
}

int RAL(Cpu_state *state) {
    uint8_t oldcy = state->cc.cy;
    state->cc.cy = (state->regs[A] & 0x80) != 0;
    state->regs[A] <<= 1;
    state->regs[A] += oldcy;
    return 4;
}

int RAR(Cpu_state *state) {
    uint8_t oldcy = state->cc.cy;
    state->cc.cy = (state->regs[A] & 0x01) != 0;
    state->regs[A] >>= 1;
    state->regs[A] += (oldcy * 0x80);
    return 4;
}

int PUSH(Cpu_state *state, uint8_t reg) {
    uint8_t byte1, byte2;
    if (reg == PSW) {
        byte1 = state->regs[A];
        byte2 = (state->cc.s << 7) | (state->cc.z << 6) | (state->cc.ac << 4)
              | (state->cc.p << 2) | (1 << 1) | (state->cc.cy);
    } else {
        byte1 = state->regs[reg];
        byte2 = state->regs[reg + 1];
    }
    write_memory(state, state->sp - 1, byte1);
    write_memory(state, state->sp - 2, byte2);
    state->sp -= 2;
    return 11;
}

int POP(Cpu_state *state, uint8_t reg) {
    uint8_t byte1 = read_memory(state, state->sp + 1);
    uint8_t byte2 = read_memory(state, state->sp);
    if (reg == PSW) {
        state->cc.s  = byte2 >> 7;
        state->cc.z  = (byte2 >> 6) & 1;
        state->cc.ac = (byte2 >> 4) & 1;
        state->cc.p  = (byte2 >> 2) & 1;
        state->cc.cy = byte2 & 1;
        state->regs[A] = byte1;
    } else {
        state->regs[reg]     = byte1;
        state->regs[reg + 1] = byte2;
    }
    state->sp += 2;
    return 10;
}

int DAD(Cpu_state *state, uint8_t reg) {
    uint16_t add1 = (reg == SP) ? state->sp : (state->regs[reg] << 8) | state->regs[reg + 1];
    uint16_t add2 = (state->regs[H] << 8) | state->regs[L];
    uint32_t sum = add1 + add2;
    state->cc.cy = ((sum & 0x00010000) != 0);
    if (reg == SP) { state->sp = sum & 0xffff; }
    else { state->regs[H] = (sum >> 8) & 0xff; state->regs[L] = sum & 0xff; }
    return 10;
}

int INX(Cpu_state *state, uint8_t reg) {
    if (reg == SP) { state->sp++; }
    else { if (state->regs[reg + 1]++ == 0xff) state->regs[reg]++; }
    return 5;
}

int DCX(Cpu_state *state, uint8_t reg) {
    if (reg == SP) { state->sp--; }
    else { if (state->regs[reg + 1]-- == 0x00) state->regs[reg]--; }
    return 5;
}

int XCHG(Cpu_state *state) {
    uint8_t d = state->regs[D], e = state->regs[E];
    state->regs[D] = state->regs[H]; state->regs[E] = state->regs[L];
    state->regs[H] = d;              state->regs[L] = e;
    return 4;
}

int XTHL(Cpu_state *state) {
    uint8_t l = state->regs[L], h = state->regs[H];
    state->regs[L] = read_memory(state, state->sp);
    state->regs[H] = read_memory(state, state->sp + 1);
    write_memory(state, state->sp,     l);
    write_memory(state, state->sp + 1, h);
    return 18;
}

int SPHL(Cpu_state *state) { state->sp = (state->regs[H] << 8) | state->regs[L]; return 5; }

int LXI(Cpu_state *state, uint8_t reg) {
    uint8_t lo = read_memory(state, state->pc + 1);
    uint8_t hi = read_memory(state, state->pc + 2);
    if (reg == SP) { state->sp = (hi << 8) | lo; }
    else { state->regs[reg] = hi; state->regs[reg + 1] = lo; }
    state->pc += 2;
    return 10;
}

int MVI(Cpu_state *state, uint8_t reg) {
    int cyc = 7;
    uint8_t byte = read_memory(state, state->pc + 1);
    if (reg == M) { cyc = 10; write_memory(state, get_m_address(state, H, L), byte); }
    else           { state->regs[reg] = byte; }
    state->pc++;
    return cyc;
}

int ADI(Cpu_state *state) {
    uint8_t add1 = state->regs[A];
    uint8_t add2 = read_memory(state, state->pc + 1);
    state->cc.ac = (((add1 & 0x0f) + (add2 & 0x0f)) & 0xf0) != 0;
    uint16_t res = add1 + add2;
    state->cc.cy = (res & 0x0f00) != 0;
    state->regs[A] = res & 0xff;
    set_zsp(state, state->regs[A]);
    state->pc++;
    return 7;
}

int ACI(Cpu_state *state) {
    uint8_t add1 = state->regs[A];
    uint8_t add2 = read_memory(state, state->pc + 1);
    state->cc.ac = (((add1 & 0x0f) + (add2 & 0x0f) + 1) & 0xf0) != 0;
    uint16_t res = add1 + add2 + 1;
    state->cc.cy = (res & 0x0f00) != 0;
    state->regs[A] = res & 0xff;
    set_zsp(state, state->regs[A]);
    state->pc++;
    return 7;
}

int SUI(Cpu_state *state) {
    uint8_t sub1 = state->regs[A];
    uint8_t sub2 = ~read_memory(state, state->pc + 1);
    state->cc.ac = (((sub1 & 0x0f) + (sub2 & 0x0f) + 1) & 0xf0) != 0;
    uint16_t res = sub1 + sub2 + 1;
    state->cc.cy = (res & 0x0f00) == 0;
    state->regs[A] = res & 0xff;
    set_zsp(state, state->regs[A]);
    state->pc++;
    return 7;
}

int SBI(Cpu_state *state) {
    uint8_t sub1 = state->regs[A];
    uint8_t sub2 = ~(read_memory(state, state->pc + 1) + state->cc.cy);
    state->cc.ac = (((sub1 & 0x0f) + (sub2 & 0x0f) + 1) & 0xf0) != 0;
    uint16_t res = sub1 + sub2 + 1;
    state->cc.cy = (res & 0x0f00) == 0;
    state->regs[A] = res & 0xff;
    set_zsp(state, state->regs[A]);
    state->pc++;
    return 7;
}

int ANI(Cpu_state *state) {
    uint8_t and = read_memory(state, state->pc + 1);
    state->cc.ac = ((state->regs[A] | and) & 0x08) != 0;
    state->regs[A] &= and;
    state->cc.cy = 0;
    set_zsp(state, state->regs[A]);
    state->pc++;
    return 7;
}

int XRI(Cpu_state *state) {
    state->regs[A] ^= read_memory(state, state->pc + 1);
    state->cc.cy = 0;
    set_zsp(state, state->regs[A]);
    state->pc++;
    return 7;
}

int ORI(Cpu_state *state) {
    state->regs[A] |= read_memory(state, state->pc + 1);
    state->cc.cy = 0;
    state->cc.ac = 0;
    set_zsp(state, state->regs[A]);
    state->pc++;
    return 7;
}

int CPI(Cpu_state *state) {
    uint8_t cmp1 = state->regs[A];
    uint8_t cmp2 = ~read_memory(state, state->pc + 1);
    int16_t sub = cmp1 - ~cmp2;
    state->cc.ac = (~(cmp1 ^ sub ^ ~cmp2) & 0x10) != 0;
    uint16_t res = cmp1 + cmp2 + 1;
    state->cc.cy = (res & 0x0f00) == 0;
    set_zsp(state, res & 0xff);
    state->pc++;
    return 7;
}

int STA(Cpu_state *state) {
    write_memory(state, get_immediate_address(state), state->regs[A]);
    state->pc += 2; return 13;
}

int LDA(Cpu_state *state) {
    state->regs[A] = read_memory(state, get_immediate_address(state));
    state->pc += 2; return 13;
}

int SHLD(Cpu_state *state) {
    uint16_t addr = get_immediate_address(state);
    write_memory(state, addr,     state->regs[L]);
    write_memory(state, addr + 1, state->regs[H]);
    state->pc += 2; return 16;
}

int LHLD(Cpu_state *state) {
    uint16_t addr = get_immediate_address(state);
    state->regs[L] = read_memory(state, addr);
    state->regs[H] = read_memory(state, addr + 1);
    state->pc += 2; return 16;
}

int PCHL(Cpu_state *state) { state->pc = get_m_address(state, H, L) - 1; return 5; }

int JMP(Cpu_state *state) { state->pc = get_immediate_address(state) - 1; return 10; }

int JC(Cpu_state *state)  { if  (state->cc.cy) JMP(state); else state->pc += 2; return 10; }
int JNC(Cpu_state *state) { if (!state->cc.cy) JMP(state); else state->pc += 2; return 10; }
int JZ(Cpu_state *state)  { if  (state->cc.z)  JMP(state); else state->pc += 2; return 10; }
int JNZ(Cpu_state *state) { if (!state->cc.z)  JMP(state); else state->pc += 2; return 10; }
int JM(Cpu_state *state)  { if  (state->cc.s)  JMP(state); else state->pc += 2; return 10; }
int JP(Cpu_state *state)  { if (!state->cc.s)  JMP(state); else state->pc += 2; return 10; }
int JPE(Cpu_state *state) { if  (state->cc.p)  JMP(state); else state->pc += 2; return 10; }
int JPO(Cpu_state *state) { if (!state->cc.p)  JMP(state); else state->pc += 2; return 10; }

int CALL(Cpu_state *state) {
    uint16_t ret = state->pc + 3;
    write_memory(state, state->sp - 1, ret >> 8);
    write_memory(state, state->sp - 2, ret & 0xff);
    state->sp -= 2;
    JMP(state);
    return 17;
}

int CC(Cpu_state *state)  { if  (state->cc.cy) return CALL(state); state->pc += 2; return 11; }
int CNC(Cpu_state *state) { if (!state->cc.cy) return CALL(state); state->pc += 2; return 11; }
int CZ(Cpu_state *state)  { if  (state->cc.z)  return CALL(state); state->pc += 2; return 11; }
int CNZ(Cpu_state *state) { if (!state->cc.z)  return CALL(state); state->pc += 2; return 11; }
int CM(Cpu_state *state)  { if  (state->cc.s)  return CALL(state); state->pc += 2; return 11; }
int CP(Cpu_state *state)  { if (!state->cc.s)  return CALL(state); state->pc += 2; return 11; }
int CPE(Cpu_state *state) { if  (state->cc.p)  return CALL(state); state->pc += 2; return 11; }
int CPO(Cpu_state *state) { if (!state->cc.p)  return CALL(state); state->pc += 2; return 11; }

int RET(Cpu_state *state) {
    state->pc = read_memory(state, state->sp) | (read_memory(state, state->sp + 1) << 8);
    state->pc--;
    state->sp += 2;
    return 10;
}

int RC(Cpu_state *state)  { if  (state->cc.cy) return RET(state) + 1; return 5; }
int RNC(Cpu_state *state) { if (!state->cc.cy) return RET(state) + 1; return 5; }
int RZ(Cpu_state *state)  { if  (state->cc.z)  return RET(state) + 1; return 5; }
int RNZ(Cpu_state *state) { if (!state->cc.z)  return RET(state) + 1; return 5; }
int RM(Cpu_state *state)  { if  (state->cc.s)  return RET(state) + 1; return 5; }
int RP(Cpu_state *state)  { if (!state->cc.s)  return RET(state) + 1; return 5; }
int RPE(Cpu_state *state) { if  (state->cc.p)  return RET(state) + 1; return 5; }
int RPO(Cpu_state *state) { if (!state->cc.p)  return RET(state) + 1; return 5; }

int RST(Cpu_state *state, uint16_t offset) {
    uint16_t ret = state->pc + 3;
    write_memory(state, state->sp - 1, ret >> 8);
    write_memory(state, state->sp - 2, ret & 0xff);
    state->sp -= 2;
    state->pc = offset << 3;
    return 11;
}

int EI(Cpu_state *state)  { state->int_enable = 1; return 4; }
int DI(Cpu_state *state)  { state->int_enable = 0; return 4; }

int IN(Cpu_state *state) {
    state->pc++;
    state->regs[A] = read_port(read_memory(state, state->pc));
    return 10;
}

int OUT(Cpu_state *state) {
    state->pc++;
    write_port(read_memory(state, state->pc), state->regs[A]);
    return 10;
}

int HLT(Cpu_state *state) {
    // HLT halts until interrupt. In our cycle-count emulator, keep advancing
    // cycles so the interrupt threshold fires and breaks out of the inner loop.
    (void)state;
    return 7;
}

uint8_t read_memory(Cpu_state *state, uint16_t address) {
    if (address >= 0x6000) return 0;
    if (address >= 0x4000 && address < 0x6000) address -= 0x2000;
    return state->memory[address];
}

void write_memory(Cpu_state *state, uint16_t address, uint8_t value) {
    if (address >= 0x2000) {
        if (address >= 0x4000 && address < 0x6000) address -= 0x2000;
        state->memory[address] = value;
    }
}

int interrupt(Cpu_state *state, uint16_t offset) {
    if (state->int_enable) {
        state->pc -= 3;
        state->int_enable = 0;
        return RST(state, offset);
    }
    return 0;
}

int exec_opcode(Cpu_state *state) {
    uint8_t op = read_memory(state, state->pc);
    int cyc = 0;

    switch (op) {
    case 0x00: cyc = 4;                    break;
    case 0x01: cyc = LXI(state, B);        break;
    case 0x02: cyc = STAX(state, B);       break;
    case 0x03: cyc = INX(state, B);        break;
    case 0x04: cyc = INR(state, B);        break;
    case 0x05: cyc = DCR(state, B);        break;
    case 0x06: cyc = MVI(state, B);        break;
    case 0x07: cyc = RLC(state);           break;
    case 0x08: cyc = 4;                    break; // undocumented — NOP on real 8080
    case 0x09: cyc = DAD(state, B);        break;
    case 0x0a: cyc = LDAX(state, B);       break;
    case 0x0b: cyc = DCX(state, B);        break;
    case 0x0c: cyc = INR(state, C);        break;
    case 0x0d: cyc = DCR(state, C);        break;
    case 0x0e: cyc = MVI(state, C);        break;
    case 0x0f: cyc = RRC(state);           break;
    case 0x10: cyc = 4;                    break; // undocumented — NOP on real 8080
    case 0x11: cyc = LXI(state, D);        break;
    case 0x12: cyc = STAX(state, D);       break;
    case 0x13: cyc = INX(state, D);        break;
    case 0x14: cyc = INR(state, D);        break;
    case 0x15: cyc = DCR(state, D);        break;
    case 0x16: cyc = MVI(state, D);        break;
    case 0x17: cyc = RAL(state);           break;
    case 0x18: cyc = 4;                    break; // undocumented — NOP on real 8080
    case 0x19: cyc = DAD(state, D);        break;
    case 0x1a: cyc = LDAX(state, D);       break;
    case 0x1b: cyc = DCX(state, D);        break;
    case 0x1c: cyc = INR(state, E);        break;
    case 0x1d: cyc = DCR(state, E);        break;
    case 0x1e: cyc = MVI(state, E);        break;
    case 0x1f: cyc = RAR(state);           break;
    case 0x20: cyc = 4;                    break; // undocumented — NOP on real 8080
    case 0x21: cyc = LXI(state, H);        break;
    case 0x22: cyc = SHLD(state);          break;
    case 0x23: cyc = INX(state, H);        break;
    case 0x24: cyc = INR(state, H);        break;
    case 0x25: cyc = DCR(state, H);        break;
    case 0x26: cyc = MVI(state, H);        break;
    case 0x27: cyc = DAA(state);           break;
    case 0x28: cyc = 4;                    break; // undocumented — NOP on real 8080
    case 0x29: cyc = DAD(state, H);        break;
    case 0x2a: cyc = LHLD(state);          break;
    case 0x2b: cyc = DCX(state, H);        break;
    case 0x2c: cyc = INR(state, L);        break;
    case 0x2d: cyc = DCR(state, L);        break;
    case 0x2e: cyc = MVI(state, L);        break;
    case 0x2f: cyc = CMA(state);           break;
    case 0x30: cyc = 4;                    break; // undocumented — NOP on real 8080
    case 0x31: cyc = LXI(state, SP);       break;
    case 0x32: cyc = STA(state);           break;
    case 0x33: cyc = INX(state, SP);       break;
    case 0x34: cyc = INR(state, M);        break;
    case 0x35: cyc = DCR(state, M);        break;
    case 0x36: cyc = MVI(state, M);        break;
    case 0x37: cyc = STC(state);           break;
    case 0x38: cyc = 4;                    break; // undocumented — NOP on real 8080
    case 0x39: cyc = DAD(state, SP);       break;
    case 0x3a: cyc = LDA(state);           break;
    case 0x3b: cyc = DCX(state, SP);       break;
    case 0x3c: cyc = INR(state, A);        break;
    case 0x3d: cyc = DCR(state, A);        break;
    case 0x3e: cyc = MVI(state, A);        break;
    case 0x3f: cyc = CMC(state);           break;
    case 0x40: cyc = MOV(state, B, B);     break;
    case 0x41: cyc = MOV(state, B, C);     break;
    case 0x42: cyc = MOV(state, B, D);     break;
    case 0x43: cyc = MOV(state, B, E);     break;
    case 0x44: cyc = MOV(state, B, H);     break;
    case 0x45: cyc = MOV(state, B, L);     break;
    case 0x46: cyc = MOV(state, B, M);     break;
    case 0x47: cyc = MOV(state, B, A);     break;
    case 0x48: cyc = MOV(state, C, B);     break;
    case 0x49: cyc = MOV(state, C, C);     break;
    case 0x4a: cyc = MOV(state, C, D);     break;
    case 0x4b: cyc = MOV(state, C, E);     break;
    case 0x4c: cyc = MOV(state, C, H);     break;
    case 0x4d: cyc = MOV(state, C, L);     break;
    case 0x4e: cyc = MOV(state, C, M);     break;
    case 0x4f: cyc = MOV(state, C, A);     break;
    case 0x50: cyc = MOV(state, D, B);     break;
    case 0x51: cyc = MOV(state, D, C);     break;
    case 0x52: cyc = MOV(state, D, D);     break;
    case 0x53: cyc = MOV(state, D, E);     break;
    case 0x54: cyc = MOV(state, D, H);     break;
    case 0x55: cyc = MOV(state, D, L);     break;
    case 0x56: cyc = MOV(state, D, M);     break;
    case 0x57: cyc = MOV(state, D, A);     break;
    case 0x58: cyc = MOV(state, E, B);     break;
    case 0x59: cyc = MOV(state, E, C);     break;
    case 0x5a: cyc = MOV(state, E, D);     break;
    case 0x5b: cyc = MOV(state, E, E);     break;
    case 0x5c: cyc = MOV(state, E, H);     break;
    case 0x5d: cyc = MOV(state, E, L);     break;
    case 0x5e: cyc = MOV(state, E, M);     break;
    case 0x5f: cyc = MOV(state, E, A);     break;
    case 0x60: cyc = MOV(state, H, B);     break;
    case 0x61: cyc = MOV(state, H, C);     break;
    case 0x62: cyc = MOV(state, H, D);     break;
    case 0x63: cyc = MOV(state, H, E);     break;
    case 0x64: cyc = MOV(state, H, H);     break;
    case 0x65: cyc = MOV(state, H, L);     break;
    case 0x66: cyc = MOV(state, H, M);     break;
    case 0x67: cyc = MOV(state, H, A);     break;
    case 0x68: cyc = MOV(state, L, B);     break;
    case 0x69: cyc = MOV(state, L, C);     break;
    case 0x6a: cyc = MOV(state, L, D);     break;
    case 0x6b: cyc = MOV(state, L, E);     break;
    case 0x6c: cyc = MOV(state, L, H);     break;
    case 0x6d: cyc = MOV(state, L, L);     break;
    case 0x6e: cyc = MOV(state, L, M);     break;
    case 0x6f: cyc = MOV(state, L, A);     break;
    case 0x70: cyc = MOV(state, M, B);     break;
    case 0x71: cyc = MOV(state, M, C);     break;
    case 0x72: cyc = MOV(state, M, D);     break;
    case 0x73: cyc = MOV(state, M, E);     break;
    case 0x74: cyc = MOV(state, M, H);     break;
    case 0x75: cyc = MOV(state, M, L);     break;
    case 0x76: cyc = HLT(state);           break;
    case 0x77: cyc = MOV(state, M, A);     break;
    case 0x78: cyc = MOV(state, A, B);     break;
    case 0x79: cyc = MOV(state, A, C);     break;
    case 0x7a: cyc = MOV(state, A, D);     break;
    case 0x7b: cyc = MOV(state, A, E);     break;
    case 0x7c: cyc = MOV(state, A, H);     break;
    case 0x7d: cyc = MOV(state, A, L);     break;
    case 0x7e: cyc = MOV(state, A, M);     break;
    case 0x7f: cyc = MOV(state, A, A);     break;
    case 0x80: cyc = ADD(state, B);        break;
    case 0x81: cyc = ADD(state, C);        break;
    case 0x82: cyc = ADD(state, D);        break;
    case 0x83: cyc = ADD(state, E);        break;
    case 0x84: cyc = ADD(state, H);        break;
    case 0x85: cyc = ADD(state, L);        break;
    case 0x86: cyc = ADD(state, M);        break;
    case 0x87: cyc = ADD(state, A);        break;
    case 0x88: cyc = ADC(state, B);        break;
    case 0x89: cyc = ADC(state, C);        break;
    case 0x8a: cyc = ADC(state, D);        break;
    case 0x8b: cyc = ADC(state, E);        break;
    case 0x8c: cyc = ADC(state, H);        break;
    case 0x8d: cyc = ADC(state, L);        break;
    case 0x8e: cyc = ADC(state, M);        break;
    case 0x8f: cyc = ADC(state, A);        break;
    case 0x90: cyc = SUB(state, B);        break;
    case 0x91: cyc = SUB(state, C);        break;
    case 0x92: cyc = SUB(state, D);        break;
    case 0x93: cyc = SUB(state, E);        break;
    case 0x94: cyc = SUB(state, H);        break;
    case 0x95: cyc = SUB(state, L);        break;
    case 0x96: cyc = SUB(state, M);        break;
    case 0x97: cyc = SUB(state, A);        break;
    case 0x98: cyc = SBB(state, B);        break;
    case 0x99: cyc = SBB(state, C);        break;
    case 0x9a: cyc = SBB(state, D);        break;
    case 0x9b: cyc = SBB(state, E);        break;
    case 0x9c: cyc = SBB(state, H);        break;
    case 0x9d: cyc = SBB(state, L);        break;
    case 0x9e: cyc = SBB(state, M);        break;
    case 0x9f: cyc = SBB(state, A);        break;
    case 0xa0: cyc = ANA(state, B);        break;
    case 0xa1: cyc = ANA(state, C);        break;
    case 0xa2: cyc = ANA(state, D);        break;
    case 0xa3: cyc = ANA(state, E);        break;
    case 0xa4: cyc = ANA(state, H);        break;
    case 0xa5: cyc = ANA(state, L);        break;
    case 0xa6: cyc = ANA(state, M);        break;
    case 0xa7: cyc = ANA(state, A);        break;
    case 0xa8: cyc = XRA(state, B);        break;
    case 0xa9: cyc = XRA(state, C);        break;
    case 0xaa: cyc = XRA(state, D);        break;
    case 0xab: cyc = XRA(state, E);        break;
    case 0xac: cyc = XRA(state, H);        break;
    case 0xad: cyc = XRA(state, L);        break;
    case 0xae: cyc = XRA(state, M);        break;
    case 0xaf: cyc = XRA(state, A);        break;
    case 0xb0: cyc = ORA(state, B);        break;
    case 0xb1: cyc = ORA(state, C);        break;
    case 0xb2: cyc = ORA(state, D);        break;
    case 0xb3: cyc = ORA(state, E);        break;
    case 0xb4: cyc = ORA(state, H);        break;
    case 0xb5: cyc = ORA(state, L);        break;
    case 0xb6: cyc = ORA(state, M);        break;
    case 0xb7: cyc = ORA(state, A);        break;
    case 0xb8: cyc = CMP(state, B);        break;
    case 0xb9: cyc = CMP(state, C);        break;
    case 0xba: cyc = CMP(state, D);        break;
    case 0xbb: cyc = CMP(state, E);        break;
    case 0xbc: cyc = CMP(state, H);        break;
    case 0xbd: cyc = CMP(state, L);        break;
    case 0xbe: cyc = CMP(state, M);        break;
    case 0xbf: cyc = CMP(state, A);        break;
    case 0xc0: cyc = RNZ(state);           break;
    case 0xc1: cyc = POP(state, B);        break;
    case 0xc2: cyc = JNZ(state);           break;
    case 0xc3: cyc = JMP(state);           break;
    case 0xc4: cyc = CNZ(state);           break;
    case 0xc5: cyc = PUSH(state, B);       break;
    case 0xc6: cyc = ADI(state);           break;
    case 0xc7: cyc = RST(state, 0);        break;
    case 0xc8: cyc = RZ(state);            break;
    case 0xc9: cyc = RET(state);           break;
    case 0xca: cyc = JZ(state);            break;
    case 0xcb: cyc = 4;                    break; // undocumented — NOP on real 8080
    case 0xcc: cyc = CZ(state);            break;
    case 0xcd: cyc = CALL(state);          break;
    case 0xce: cyc = ACI(state);           break;
    case 0xcf: cyc = RST(state, 1);        break;
    case 0xd0: cyc = RNC(state);           break;
    case 0xd1: cyc = POP(state, D);        break;
    case 0xd2: cyc = JNC(state);           break;
    case 0xd3: cyc = OUT(state);           break;
    case 0xd4: cyc = CNC(state);           break;
    case 0xd5: cyc = PUSH(state, D);       break;
    case 0xd6: cyc = SUI(state);           break;
    case 0xd7: cyc = RST(state, 2);        break;
    case 0xd8: cyc = RC(state);            break;
    case 0xd9: cyc = 4;                    break; // undocumented — NOP on real 8080
    case 0xda: cyc = JC(state);            break;
    case 0xdb: cyc = IN(state);            break;
    case 0xdc: cyc = CC(state);            break;
    case 0xdd: cyc = 4;                    break; // undocumented — NOP on real 8080
    case 0xde: cyc = SBI(state);           break;
    case 0xdf: cyc = RST(state, 3);        break;
    case 0xe0: cyc = RPO(state);           break;
    case 0xe1: cyc = POP(state, H);        break;
    case 0xe2: cyc = JPO(state);           break;
    case 0xe3: cyc = XTHL(state);          break;
    case 0xe4: cyc = CPO(state);           break;
    case 0xe5: cyc = PUSH(state, H);       break;
    case 0xe6: cyc = ANI(state);           break;
    case 0xe7: cyc = RST(state, 4);        break;
    case 0xe8: cyc = RPE(state);           break;
    case 0xe9: cyc = PCHL(state);          break;
    case 0xea: cyc = JPE(state);           break;
    case 0xeb: cyc = XCHG(state);          break;
    case 0xec: cyc = CPE(state);           break;
    case 0xed: cyc = 4;                    break; // undocumented — NOP on real 8080
    case 0xee: cyc = XRI(state);           break;
    case 0xef: cyc = RST(state, 5);        break;
    case 0xf0: cyc = RP(state);            break;
    case 0xf1: cyc = POP(state, PSW);      break;
    case 0xf2: cyc = JP(state);            break;
    case 0xf3: cyc = DI(state);            break;
    case 0xf4: cyc = CP(state);            break;
    case 0xf5: cyc = PUSH(state, PSW);     break;
    case 0xf6: cyc = ORI(state);           break;
    case 0xf7: cyc = RST(state, 6);        break;
    case 0xf8: cyc = RM(state);            break;
    case 0xf9: cyc = SPHL(state);          break;
    case 0xfa: cyc = JM(state);            break;
    case 0xfb: cyc = EI(state);            break;
    case 0xfc: cyc = CM(state);            break;
    case 0xfd: cyc = 4;                    break; // undocumented — NOP on real 8080
    case 0xfe: cyc = CPI(state);           break;
    case 0xff: cyc = RST(state, 7);        break;
    }

    state->pc++;
    return cyc;
}
