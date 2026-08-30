// Galaga 3x Z80 bus wiring -- memory map + the 06XX custom I/O mux.
//
// Memory map verified against MAME's galaga_state::galaga_map()
// (src/mame/namco/galaga.cpp, fetched and read directly this session --
// see galaga_machine.h's header comment for the full citation trail).
// All 3 CPUs (main/sub/sub2) install the literal same map function in
// MAME's source -- only the 0x0000-0x3FFF ROM bank differs per CPU (each
// Z80 device owns its own ROM region). That means WSG/misclatch/06xx/
// videolatch are addressable from all 3 CPUs at the bus level, even
// though only main CPU's game code is expected to actually use them in
// practice -- this file wires all 3 CPUs' read_byte/write_byte to the
// same shared >=0x4000 decode logic accordingly, only the ROM read at
// <0x4000 differs per CPU function.
//
//   0x0000-0x3FFF  per-CPU program ROM (read only; writes are nopw'd on
//                  real hardware, so this file no-ops them too)
//   0x4000-0x67FF  unmapped (real hardware: open bus) -- reads return
//                  0xFF, writes are ignored, same convention
//                  pacman_ports.cpp uses for its own unmapped ranges
//   0x6800-0x6807  DIP switch read (bosco_dsw_r), READ ONLY -- a bit-serial
//                  read, NOT a flat byte: address 0x6800+offset returns
//                  DSWB's bit `offset` in bit 0 and DSWA's bit `offset`
//                  in bit 1. Missed in an earlier pass of this file (the
//                  0x6800-0x681F WSG write range below overlaps it, and
//                  the read side was assumed unmapped without checking)
//                  -- real hardware bug found bringing this up: this
//                  range returned a constant 0xFF regardless of offset,
//                  so every DIP bit the game reads back looked SET (1)
//                  rather than the real per-switch value. DIP reads on
//                  this board are emphatically load-bearing, not cosmetic
//                  config: the SECOND DIP bug (wrong defaults seeded, see
//                  galaga_assets.cpp's dswa comment) is what caused the
//                  multi-session 3-CPU boot deadlock, because sub CPU's
//                  task-0x0A handler tests DSWA bit 2 via 0x6802 and, when
//                  it reads clear, falls into a path that executes `RST 0`
//                  -- resetting sub -- every single frame. Treat anything
//                  the game reads through here as capable of changing
//                  control flow, not just difficulty settings.
//   0x6800-0x681F  WSG voice registers, WRITE ONLY (namco_wsg_device::
//                  pacman_sound_w -- the read side of 0x6800-0x6807 is
//                  DIP switches, above; 0x6808-0x681F has no read mapping)
//   0x6820-0x6827  "misclatch" 74LS259, WRITE ONLY, one bit per address
//   0x6830         watchdog reset (not emulated, same precedent as every
//                  other port in this project)
//   0x6831-0x6FFF  unmapped
//   0x7000-0x70FF  06XX data register (read/write; the low address bits
//                  are "don't care" on real hardware -- the whole 256-byte
//                  span aliases the same register, same as the port map's
//                  `rw(...)` covering that whole range in one entry)
//   0x7100         06XX control register (read/write)
//   0x7101-0x7FFF  unmapped
//   0x8000-0x87FF  shared video RAM (tile numbers + sprite registers)
//   0x8800-0x8BFF  shared RAM 1
//   0x8C00-0x8FFF  unmapped
//   0x9000-0x93FF  shared RAM 2
//   0x9400-0x97FF  unmapped
//   0x9800-0x9BFF  shared RAM 3
//   0x9C00-0x9FFF  unmapped
//   0xA000-0xA007  "videolatch" 74LS259, WRITE ONLY, one bit per address
//   0xA008-0xFFFF  unmapped
//
// 06XX mux logic verified against namco06.cpp's data_r() (quoted
// verbatim in project research): control register bit 4 = read(1)/
// write(0) mode; bits 0-3 = per-chip select, AND-reduced across selected
// chips on read (0xFF if none selected), broadcast to selected chips on
// write. Galaga wires only chip-select 0 -> 51xx and chip-select 3 ->
// 54xx (namco51.cpp/namco54.cpp's HLE, see galaga_51xx.h/galaga_54xx.h)
// -- no 50xx, per the driver's own header comment ("Galaga has one
// missing RAM and no 50XX custom"). data_r() AND data_w()/write_sync()'s
// exact bodies were fetched and quoted directly this session (superseding
// an earlier pass's "write side only inferred by symmetry" note) --
// confirms this file's write-side implementation matches real hardware.
// The periodic main-CPU NMI the real 06XX generates from its clock
// divider is now implemented too (galaga_machine.h's io06_nmi_period/
// io06_nmi_next fields, this file's 0x7100 write handler, and
// galaga_machine.cpp's interleave loop) -- an earlier Phase A pass had
// skipped it for lack of the real period formula; namco06.cpp's exact
// ctrl_w_sync()/nmi_generate() source (fetched this session) provided it.
// This was found to matter on real hardware: Phase A without this NMI
// booted but got stuck showing RAM-test-pattern garbage instead of
// reaching the attract screen.
#include "galaga_ports.h"

// See galaga_machine.h's namco_busy_until field comment for the citation
// and the approximation this converts from (danjulio/gcore_galagino's
// 5000 iterations of a 4-instruction interleave loop).
#define GALAGA_NAMCO_BUSY_CYCLES 100000UL

// Memory access is the single hottest path in this machine: every emulated
// instruction fetch and every operand access on all 3 Z80s lands here. Put
// it in SRAM rather than flash for the same XIP-cache reason ArcadeCPU_Z80's
// z80.c does (see the Z80_RAMFUNC comment there for the full rationale and
// the measured frame-budget impact on the Fruit Jam).
#if defined(ARDUINO_ARCH_RP2040) || defined(PICO_ON_DEVICE)
#define GALAGA_RAMFUNC __attribute__((section(".time_critical.galaga")))
#else
#define GALAGA_RAMFUNC
#endif

// --- Shared >=0x4000 decode, used by all 3 CPUs' read_byte/write_byte ---

GALAGA_RAMFUNC static uint8_t galaga_shared_read(galaga_system *sys, uint16_t addr) {
    if (addr < 0x6800) return 0xFF; // unmapped
    if (addr < 0x6808) {
        // DIP switch read (bosco_dsw_r, verified against the actual
        // quoted galaga_map()/bosco_dsw_r() source -- NOT a flat byte
        // read like Pac-Man's dsw1): each address here returns DSWB's
        // bit `offset` in bit 0 and DSWA's bit `offset` in bit 1. Real
        // hardware's upper 6 bits of this read are left as whatever the
        // bus floats to; MAME's own function only ever sets bits 0-1, so
        // this returns exactly that (not 0xFF-padded) to match.
        uint16_t offset = (uint16_t)(addr - 0x6800);
        uint8_t bit0 = (uint8_t)((sys->dswb >> offset) & 1u);
        uint8_t bit1 = (uint8_t)((sys->dswa >> offset) & 1u);
        return (uint8_t)(bit0 | (bit1 << 1));
    }
    if (addr < 0x6820) return 0xFF; // WSG regs are write-only
    if (addr < 0x6830) return 0xFF; // misclatch is write-only
    if (addr < 0x7000) return 0xFF; // unmapped (incl. watchdog addr)
    if (addr < 0x7100) {
        // 06XX data register -- only meaningful in read mode (ctrl bit4
        // set); AND-reduce across selected chips, 0xFF if none selected.
        if (!(sys->io06_control & 0x10u)) return 0xFF; // in write mode -- real chip logs+returns 0
        uint8_t result = 0xFF;
        if (sys->io06_control & 0x01u) result &= galaga_51xx_read(&sys->io51);
        // chip-selects 1/2: no chip wired (no 50xx on this board) -- contribute nothing
        if (sys->io06_control & 0x08u) result &= 0xFF; // 54xx is write-only in this HLE
        return result;
    }
    if (addr == 0x7100) {
        // Busy/ready status, NOT an echo of io06_control -- see
        // galaga_machine.h's namco_busy_until field comment.
        bool busy = (int32_t)(sys->cpu_main.cyc - sys->namco_busy_until) < 0;
        return busy ? 0x00u : 0x10u;
    }
    if (addr < 0x8000) return 0xFF; // unmapped
    if (addr < 0x8800) return sys->video_ram[addr - 0x8000];
    if (addr < 0x8C00) return sys->ram1[addr - 0x8800];
    if (addr < 0x9000) return 0xFF; // unmapped
    if (addr < 0x9400) return sys->ram2[addr - 0x9000];
    if (addr < 0x9800) return 0xFF; // unmapped
    if (addr < 0x9C00) return sys->ram3[addr - 0x9800];
    return 0xFF; // 0x9C00-0xFFFF unmapped (incl. videolatch, write-only)
}

GALAGA_RAMFUNC static void galaga_shared_write(galaga_system *sys, uint16_t addr, uint8_t data) {
    if (addr < 0x6800) return; // unmapped
    if (addr < 0x6820) { sys->wsg_regs[addr - 0x6800] = (uint8_t)(data & 0x0Fu); return; }
    if (addr < 0x6828) {
        // misclatch -- one bit per address, LS259 write_d0 convention
        // (bit 0 of `data` sets/clears the output named by addr-0x6820).
        bool bit = (data & 0x01u) != 0;
        switch (addr - 0x6820) {
        case 0: sys->irq1_enable = bit; break;
        case 1: sys->irq2_enable = bit; break;
        case 2: sys->nmi2_enable = !bit; break; // nmion_w's inversion -- see galaga_machine.h's caveat
        case 3:
            // Capture the release instant the first (and normally only)
            // time this transitions false->true -- see galaga_machine.h's
            // reset_release_main_cyc field comment for why this matters.
            if (bit && !sys->sub_reset_released) {
                sys->reset_release_main_cyc = sys->cpu_main.cyc;
            }
            sys->sub_reset_released = bit;
            // Also resets 51xx's credit mode -- confirmed against the
            // reference implementation's own comment on this exact write
            // ("this also resets the 51xx"), see galaga_51xx.h's
            // credit_mode field comment.
            sys->io51.credit_mode = false;
            break;
        default: break; // Q4-Q7: not modeled, no wiring found in the source read this session
        }
        return;
    }
    if (addr == 0x6830) return; // watchdog reset -- not emulated
    if (addr < 0x7000) return;  // unmapped
    if (addr < 0x7100) {
        // 06XX data register -- only meaningful in write mode (ctrl bit4 clear).
        if (sys->io06_control & 0x10u) return; // in read mode -- real chip ignores writes here
        if (sys->io06_control & 0x01u) galaga_51xx_write(&sys->io51, data);
        if (sys->io06_control & 0x08u) galaga_54xx_write(&sys->io54, data);
        return;
    }
    if (addr == 0x7100) {
        sys->io06_control = data;
        // Reset the read/write position counter for whatever chip is
        // selected -- see galaga_51xx.h's namco_cnt citation
        // (danjulio/gcore_galagino's emulation.c: "namco_cnt = 0" on
        // every control write).
        sys->io51.namco_cnt = 0;
        // Busy/ready gate for the NEXT read of this same register -- see
        // galaga_machine.h's namco_busy_until field comment.
        sys->namco_busy_until = sys->cpu_main.cyc + GALAGA_NAMCO_BUSY_CYCLES;
        // 06XX NMI timer -- see galaga_machine.h's io06_nmi_period/
        // io06_nmi_next field comments for the exact formula/citation.
        if ((data & 0xE0u) == 0) {
            sys->io06_nmi_period = 0; // timer stopped
        } else {
            uint32_t divisor = 1UL << ((data >> 5) & 0x7u);
            sys->io06_nmi_period = 64UL * divisor;
            sys->io06_nmi_next = sys->cpu_main.cyc + sys->io06_nmi_period;
        }
        return;
    }
    if (addr < 0x8000) return; // unmapped
    if (addr < 0x8800) { sys->video_ram[addr - 0x8000] = data; return; }
    if (addr < 0x8C00) { sys->ram1[addr - 0x8800] = data; return; }
    if (addr < 0x9000) return; // unmapped
    if (addr < 0x9400) { sys->ram2[addr - 0x9000] = data; return; }
    if (addr < 0x9800) return; // unmapped
    if (addr < 0x9C00) { sys->ram3[addr - 0x9800] = data; return; }
    if (addr < 0xA008 && addr >= 0xA000) {
        // videolatch -- Q0-Q5 starfield control (consumed by
        // galaga_video.cpp), Q6 unknown/unwired, Q7 flip_screen.
        bool bit = (data & 0x01u) != 0;
        uint16_t q = (uint16_t)(addr - 0xA000);
        if (q <= 5) {
            if (bit) sys->starfield_control = (uint8_t)(sys->starfield_control | (1u << q));
            else     sys->starfield_control = (uint8_t)(sys->starfield_control & ~(1u << q));
        } else if (q == 7) {
            sys->flip_screen = bit;
        }
        return;
    }
    // 0xA008-0xFFFF: unmapped
}

// --- Per-CPU read_byte/write_byte: only the <0x4000 ROM bank differs ---

GALAGA_RAMFUNC static uint8_t galaga_main_read_byte(void *userdata, uint16_t addr) {
    galaga_system *sys = (galaga_system *)userdata;
    if (addr < GALAGA_ROM_MAIN_SIZE) return sys->rom_main[addr];
    return galaga_shared_read(sys, addr);
}
GALAGA_RAMFUNC static void galaga_main_write_byte(void *userdata, uint16_t addr, uint8_t data) {
    galaga_system *sys = (galaga_system *)userdata;
    if (addr < GALAGA_ROM_MAIN_SIZE) return; // ROM -- not writable
    galaga_shared_write(sys, addr, data);
}

GALAGA_RAMFUNC static uint8_t galaga_sub_read_byte(void *userdata, uint16_t addr) {
    galaga_system *sys = (galaga_system *)userdata;
    if (addr < GALAGA_ROM_SUB_SIZE) return sys->rom_sub[addr];
    if (addr < 0x4000) return 0xFF; // rest of the CPU's own ROM region is unmapped
    return galaga_shared_read(sys, addr);
}
GALAGA_RAMFUNC static void galaga_sub_write_byte(void *userdata, uint16_t addr, uint8_t data) {
    galaga_system *sys = (galaga_system *)userdata;
    if (addr < 0x4000) return; // ROM region -- not writable
    galaga_shared_write(sys, addr, data);
}

GALAGA_RAMFUNC static uint8_t galaga_sub2_read_byte(void *userdata, uint16_t addr) {
    galaga_system *sys = (galaga_system *)userdata;
    if (addr < GALAGA_ROM_SUB2_SIZE) return sys->rom_sub2[addr];
    if (addr < 0x4000) return 0xFF;
    return galaga_shared_read(sys, addr);
}
GALAGA_RAMFUNC static void galaga_sub2_write_byte(void *userdata, uint16_t addr, uint8_t data) {
    galaga_system *sys = (galaga_system *)userdata;
    if (addr < 0x4000) return;
    galaga_shared_write(sys, addr, data);
}

// I/O address space (Z80 IN/OUT instructions) -- not used by the "galaga"
// ROM set on any of the 3 CPUs (unlike ArcadeMachine_Pacman's main CPU,
// which uses port 0 to set its IM0 interrupt vector byte -- Galaga's
// misclatch-driven IRQs don't need that, see galaga_machine.cpp).
static uint8_t galaga_port_in(z80 *cpu, uint8_t port) {
    (void)cpu; (void)port;
    return 0xFF;
}
static void galaga_port_out(z80 *cpu, uint8_t port, uint8_t data) {
    (void)cpu; (void)port; (void)data;
}

void galaga_ports_wire(galaga_system *system) {
    system->cpu_main.read_byte  = galaga_main_read_byte;
    system->cpu_main.write_byte = galaga_main_write_byte;
    system->cpu_main.port_in    = galaga_port_in;
    system->cpu_main.port_out   = galaga_port_out;
    system->cpu_main.userdata   = system;

    system->cpu_sub.read_byte  = galaga_sub_read_byte;
    system->cpu_sub.write_byte = galaga_sub_write_byte;
    system->cpu_sub.port_in    = galaga_port_in;
    system->cpu_sub.port_out   = galaga_port_out;
    system->cpu_sub.userdata   = system;

    system->cpu_sub2.read_byte  = galaga_sub2_read_byte;
    system->cpu_sub2.write_byte = galaga_sub2_write_byte;
    system->cpu_sub2.port_in    = galaga_port_in;
    system->cpu_sub2.port_out   = galaga_port_out;
    system->cpu_sub2.userdata   = system;
}
