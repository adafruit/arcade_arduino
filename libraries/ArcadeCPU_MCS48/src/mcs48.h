// Intel MCS-48 CPU interpreter (8035/8039/8048 family, and the Fujitsu
// MB8884 clone Donkey Kong's sound board actually carries).
//
// The project's THIRD CPU axis, alongside ArcadeCPU_i8080 and ArcadeCPU_Z80.
// No hardware or game knowledge lives here.
//
// Transcribed from MAME's own mcs48 device (src/devices/cpu/mcs48/mcs48.cpp,
// upstream mamedev/mame): the opcode table, per-opcode machine-cycle counts,
// the timer/counter prescaler, and the interrupt model all come from that
// source rather than from a datasheet reading, so behaviour matches the
// emulator these games are verified against.
//
// FOUR THINGS ABOUT THIS ARCHITECTURE THAT SURPRISE PEOPLE COMING FROM THE
// Z80 OR 8080, each of which is a silent-wrong-behaviour trap rather than a
// crash:
//
//  1. THE PC IS 11 BITS PLUS A BANK BIT. Increment wraps within a 2KB page:
//     `pc = ((pc + 1) & 0x7ff) | (pc & 0x800)`. Code running at 0x7FF does
//     NOT fall through to 0x800; it wraps to 0x000. `SEL MB0`/`SEL MB1` set
//     the bank bit that JMP/CALL apply.
//  2. THE REGISTERS LIVE IN RAM. R0-R7 are RAM bytes 0-7 or 24-31 depending
//     on PSW bit 4 (`SEL RB0`/`SEL RB1`), and the stack is RAM bytes 8-23.
//     A program that writes RAM through @R0 can and does clobber its own
//     registers -- that is normal, not a bug to defend against.
//  3. THE STACK IS 8 LEVELS AND WRAPS SILENTLY. The stack pointer is the
//     low 3 bits of PSW; a 9th nested CALL overwrites the first frame.
//  4. `MOVX A,@Rn` AND `INS A,BUS` ARE DIFFERENT PATHS, and a board can
//     decode both onto the same chip while passing different addresses.
//     Donkey Kong does exactly that: `INS A,BUS` fetches a sound command,
//     while `MOVX A,@R0` reads a sample byte with R0 as the address.
//     Stubbing MOVX out costs you every sampled sound and nothing else --
//     the music still plays, so it looks like a working sound board.
#ifndef MCS48_H
#define MCS48_H

#include <stdint.h>
#include <stdbool.h>

// mcs48.c is compiled as plain C (like ArcadeCPU_i8080's i8080.c and
// ArcadeCPU_Z80's z80.c). extern "C" here is only for C++ callers.
#ifdef __cplusplus
extern "C" {
#endif

struct mcs48;

// Hardware access, wired per instance -- the same shape ArcadeCPU_Z80 uses,
// so one core can serve several machines in a single build.
typedef uint8_t (*mcs48_read_cb)(struct mcs48 *cpu, uint16_t addr);
typedef uint8_t (*mcs48_bus_read_cb)(struct mcs48 *cpu);
typedef void    (*mcs48_bus_write_cb)(struct mcs48 *cpu, uint8_t data);
typedef uint8_t (*mcs48_port_read_cb)(struct mcs48 *cpu, uint8_t port);
typedef void    (*mcs48_port_write_cb)(struct mcs48 *cpu, uint8_t port, uint8_t data);
typedef uint8_t (*mcs48_test_read_cb)(struct mcs48 *cpu, uint8_t line);
// External data space, reached by MOVX A,@Rn / MOVX @Rn,A with the register
// supplying an 8-bit address. Separate from both the program space and the
// BUS port, though a board may well decode them onto the same chip -- Donkey
// Kong's sample ROM is read this way.
typedef uint8_t (*mcs48_ext_read_cb)(struct mcs48 *cpu, uint8_t addr);
typedef void    (*mcs48_ext_write_cb)(struct mcs48 *cpu, uint8_t addr, uint8_t data);

// 8035/MB8884 internal RAM. The 8039/8049 have 128 and the 8040/8050 have
// 256; this core allocates the largest and masks to `ram_mask`, so one
// build can host different family members.
#define MCS48_RAM_SIZE 256

typedef struct mcs48 {
    uint8_t  a;        // accumulator
    uint8_t  psw;      // C(0x80) AC(0x40) F0(0x20) BS(0x10) | 3-bit stack pointer
    uint16_t pc;       // 11 bits + bank bit 0x800 -- see header note 1
    uint16_t a11;      // 0 or 0x800: the bank SEL MB0/MB1 selected

    uint8_t  ram[MCS48_RAM_SIZE];
    uint8_t  ram_mask; // 0x3F for an 8035/MB8884 (64 bytes)
    uint8_t  regptr;   // 0 or 24 -- base of R0-R7 within ram[]

    uint8_t  p1, p2;   // output latches for ports 1 and 2
    bool     f1;       // the second user flag (F0 lives in PSW)

    uint8_t  timer;    // 8-bit timer/counter
    uint8_t  prescaler;
    uint8_t  timecount_enabled; // MCS48_TIMER_ENABLED / MCS48_COUNTER_ENABLED
    uint8_t  t1_history;        // edge detection for counter mode
    bool     timer_flag;        // set on overflow, tested and cleared by JTF
    bool     timer_overflow;    // pending timer interrupt

    bool     tirq_enabled, xirq_enabled;
    bool     irq_state;       // external /INT line level, set by the machine
    bool     irq_in_progress;
    bool     irq_polled;      // JNI executed with no IRQ -- see check_irqs

    // Free-running MACHINE-cycle count (not oscillator clocks). The MCS-48
    // divides its crystal by 15, so a 6MHz part like Donkey Kong's runs
    // 400,000 of these per second. Never reset by this core; compare
    // ELAPSED cycles by subtraction, never an absolute target, so wraparound
    // stays harmless (DEVNOTES.md problems #22/#26/#27 are what that rule
    // is made of).
    uint32_t cyc;

    mcs48_read_cb       program_r; // program ROM fetch, 12-bit address
    mcs48_bus_read_cb   bus_r;     // INS A,BUS
    mcs48_bus_write_cb  bus_w;     // OUTL BUS,A / ANL BUS / ORL BUS
    mcs48_port_read_cb  port_r;    // IN A,Pp   (port is 1 or 2)
    mcs48_port_write_cb port_w;    // OUTL Pp,A (port is 1 or 2)
    mcs48_test_read_cb  test_r;    // T0 (line 0) / T1 (line 1)
    mcs48_ext_read_cb   ext_r;     // MOVX A,@Rn  (NULL -> reads 0xFF)
    mcs48_ext_write_cb  ext_w;     // MOVX @Rn,A  (NULL -> discarded)

    void *userdata;
} mcs48;

#define MCS48_TIMER_ENABLED   0x01
#define MCS48_COUNTER_ENABLED 0x02

// Resets the core to its power-on state and sets the internal RAM size
// (64, 128 or 256). Callbacks and userdata must be assigned by the caller
// before the first mcs48_step(); they are NOT cleared here, so a reset in
// the middle of a run keeps its wiring.
void mcs48_init(mcs48 *cpu, uint16_t ram_size);

// Resets the CPU without touching RAM or the callback wiring -- the
// behaviour of a real /RESET pulse, which does not clear internal RAM.
void mcs48_reset(mcs48 *cpu);

// Executes one instruction (servicing a pending interrupt first if one is
// due) and advances `cyc` by the machine cycles it consumed. Returns those
// cycles.
uint8_t mcs48_step(mcs48 *cpu);

// Sets the external /INT line level. The interrupt is level-triggered and
// is taken only while EN I is in effect, vectoring to 0x03.
void mcs48_set_irq(mcs48 *cpu, bool state);

#ifdef __cplusplus
}
#endif

#endif
