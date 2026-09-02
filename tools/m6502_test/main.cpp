// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Conformance runner for ArcadeCPU_M6502 -- runs the standard 6502 test
// suites against the vendored core and reports PASS/FAIL plus the exact
// cycle count.
//
// WHY THIS EXISTS AS ITS OWN TOOL. Every other harness in tools/ runs a
// whole arcade machine; this one runs only the CPU. A CPU bug does not
// present as a CPU bug -- it presents as wrong graphics, a hang, a game
// that boots and then behaves oddly, i.e. as every other class of bug at
// once. Burger Time is this project's first 6502 machine, so buying
// certainty about the interpreter BEFORE building a machine on top of it
// removes the most expensive possible suspect from every later
// investigation. It costs one command.
//
// The pass conditions and expected cycle counts below are upstream's, from
// superzazu/6502's own m6502_tests.c -- so a mismatch here means this
// vendored copy diverges from the upstream that reported those results,
// which is exactly the question worth asking after modifying a vendored
// core (see m6502.h's list of changes).
//
// THE TEST BINARIES ARE NOT IN THIS REPO -- they are third-party test
// programs with their own licences and are 64KB of binary. Fetch them once:
//
//   mkdir -p programs && cd programs
//   curl -LO https://raw.githubusercontent.com/Klaus2m5/6502_65C02_functional_tests/master/bin_files/6502_functional_test.bin
//   curl -LO https://raw.githubusercontent.com/superzazu/6502/master/programs/6502_decimal_test.bin
//   curl -LO https://raw.githubusercontent.com/superzazu/6502/master/programs/AllSuiteA.bin
//
// then: ./m6502_test [dir]      (dir defaults to ./programs)
//
// Credits: 6502_functional_test is Klaus Dormann's
// (github.com/Klaus2m5/6502_65C02_functional_tests, GPLv3 -- run, not
// linked, and not redistributed here); 6502_decimal_test is Bruce Clark's;
// AllSuiteA comes from the hmc-6502 project.
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

#include "m6502.h"

#define MEMORY_SIZE 0x10000
static uint8_t memory[MEMORY_SIZE];
static m6502 cpu;

static uint8_t rb(void *, uint16_t addr) { return memory[addr]; }
static void wb(void *, uint16_t addr, uint8_t val) { memory[addr] = val; }

static bool load(const std::string &path, uint16_t addr) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        fprintf(stderr, "  cannot open %s\n", path.c_str());
        return false;
    }
    size_t n = fread(&memory[addr], 1, MEMORY_SIZE - addr, f);
    fclose(f);
    return n > 0;
}

static void reset_cpu(uint16_t pc) {
    m6502_init(&cpu);
    cpu.read_byte  = &rb;
    cpu.write_byte = &wb;
    cpu.pc = pc;
}

// Reports one result. `expected_cyc` is upstream's figure; a cycle
// difference with a PASS is not a correctness failure but IS a divergence
// worth knowing about, so it is printed either way rather than asserted on.
static int report(const char *name, bool pass, uint32_t cyc,
                  uint32_t expected_cyc, unsigned long instructions) {
    printf("%-24s %s  (%lu instructions, %lu cycles, upstream %lu, diff %ld)\n",
           name, pass ? "PASS" : "FAIL", instructions,
           (unsigned long)cyc, (unsigned long)expected_cyc,
           (long)cyc - (long)expected_cyc);
    return pass ? 0 : 1;
}

// Klaus Dormann's 6502_functional_test: loaded at 0, entered at 0x0400,
// every failure is a `jmp *` trap. Success is its own distinct trap
// address, so "stopped moving" plus "stopped at 0x3469" is the pass.
static int test_functional(const std::string &dir) {
    memset(memory, 0, sizeof(memory));
    if (!load(dir + "/6502_functional_test.bin", 0x0000)) return 1;
    reset_cpu(0x0400);

    unsigned long instr = 0;
    uint16_t prev_pc = 0xFFFF;
    while (cpu.pc != prev_pc) {
        prev_pc = cpu.pc;
        m6502_step(&cpu);
        instr++;
        if (instr > 200000000UL) { // the whole suite is ~30M instructions
            printf("%-24s FAIL  (never trapped -- runaway)\n",
                   "6502_functional_test");
            return 1;
        }
    }
    bool pass = (cpu.pc == 0x3469);
    if (!pass) printf("  trapped at 0x%04X\n", cpu.pc);
    return report("6502_functional_test", pass, cpu.cyc, 96241367u, instr);
}

// Bruce Clark's decimal test: loaded at 0x0200, entered there, done when it
// reaches 0x024B with A = 0 (A holds the error count).
static int test_decimal(const std::string &dir) {
    memset(memory, 0, sizeof(memory));
    if (!load(dir + "/6502_decimal_test.bin", 0x0200)) return 1;
    reset_cpu(0x0200);

    unsigned long instr = 0;
    while (cpu.pc != 0x024B) {
        m6502_step(&cpu);
        if (++instr > 200000000UL) {
            printf("%-24s FAIL  (never reached 0x024B)\n", "6502_decimal_test");
            return 1;
        }
    }
    return report("6502_decimal_test", cpu.a == 0, cpu.cyc, 46089505u, instr);
}

// AllSuiteA: loaded at 0x4000, run from the reset vector, done at 0x45C0
// with 0xFF at 0x0210 meaning all sub-tests passed.
static int test_allsuitea(const std::string &dir) {
    memset(memory, 0, sizeof(memory));
    if (!load(dir + "/AllSuiteA.bin", 0x4000)) return 1;
    m6502_init(&cpu);
    cpu.read_byte  = &rb;
    cpu.write_byte = &wb;
    m6502_gen_res(&cpu);

    unsigned long instr = 0;
    while (cpu.pc != 0x45C0) {
        m6502_step(&cpu);
        if (++instr > 10000000UL) {
            printf("%-24s FAIL  (never reached 0x45C0)\n", "AllSuiteA");
            return 1;
        }
    }
    return report("AllSuiteA", memory[0x0210] == 0xFF, cpu.cyc, 1946u, instr);
}

// The CPU-7 hook (m6502.h change 3) is Burger-Time-specific, but whether it
// works at all is a property of the CPU library, so it is tested here rather
// than left to be discovered inside a machine that also has 15 other new
// things in it. Runs a tiny hand-built program twice -- once with
// read_opcode unset, once with a read_opcode that rewrites the fetch -- and
// checks that (a) the default path is identical to read_byte and (b) the
// hook affects ONLY instruction fetches, not operand or data reads.
static unsigned long hook_fetches = 0;
static uint8_t hook_ro(void *, uint16_t addr) {
    hook_fetches++;
    // Rewrite LDA #imm (0xA9) into LDX #imm (0xA2) on fetch only. If the
    // hook were wrongly used for the operand read too, the immediate value
    // would come back mangled as well and the checks below would catch it.
    uint8_t v = memory[addr];
    return v == 0xA9 ? 0xA2 : v;
}

static int test_opcode_hook(void) {
    // LDA #$37 ; STA $0300 ; JMP *
    const uint8_t prog[] = {0xA9, 0x37, 0x8D, 0x00, 0x03, 0x4C, 0x05, 0x02};

    memset(memory, 0, sizeof(memory));
    memcpy(&memory[0x0200], prog, sizeof(prog));
    reset_cpu(0x0200);
    for (int i = 0; i < 2; i++) m6502_step(&cpu);
    bool plain_ok = (cpu.a == 0x37 && memory[0x0300] == 0x37 && cpu.x == 0);

    memset(memory, 0, sizeof(memory));
    memcpy(&memory[0x0200], prog, sizeof(prog));
    reset_cpu(0x0200);
    cpu.read_opcode = &hook_ro;
    hook_fetches = 0;
    for (int i = 0; i < 2; i++) m6502_step(&cpu);
    // With the fetch rewritten, X (not A) takes 0x37, and the STA still
    // stores A -- which is now 0. The immediate operand must be intact,
    // proving the hook did not touch the operand read.
    bool hooked_ok = (cpu.x == 0x37 && cpu.a == 0x00 &&
                      memory[0x0300] == 0x00 && hook_fetches == 2);

    printf("%-24s %s  (default path %s, fetch-only %s, %lu fetches hooked)\n",
           "read_opcode hook", (plain_ok && hooked_ok) ? "PASS" : "FAIL",
           plain_ok ? "ok" : "BROKEN", hooked_ok ? "ok" : "BROKEN",
           hook_fetches);
    return (plain_ok && hooked_ok) ? 0 : 1;
}

int main(int argc, char **argv) {
    std::string dir = (argc > 1) ? argv[1] : "programs";

    int failures = 0;
    failures += test_opcode_hook();   // no ROM needed, so run it first
    failures += test_allsuitea(dir);
    failures += test_decimal(dir);
    failures += test_functional(dir); // slowest, ~30M instructions

    printf("\nillegal opcodes executed during the last run: %lu\n",
           (unsigned long)cpu.illegal_ops);
    printf("%s\n", failures == 0 ? "all tests passed" : "FAILURES PRESENT");
    return failures != 0;
}
