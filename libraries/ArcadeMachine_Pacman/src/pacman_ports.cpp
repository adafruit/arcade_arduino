// Pac-Man Z80 bus wiring -- memory map + the one real I/O-space write.
//
// Memory map (verified against MAME's pacman_state::pacman_map(),
// src/mame/pacman/pacman.cpp -- the "mirror(...)" masks there just alias
// these same ranges into unused high address bits on real incompletely-
// decoded hardware; real Pac-Man ROM code never relies on that aliasing
// the way Space Invaders' self-test does (see ArcadeCPU_i8080's
// mirror_2000_at_4000 comment), so it is deliberately NOT replicated here
// -- only the direct ranges below are backed by real storage):
//
//   0x0000-0x3FFF  ROM (4x 4K chips, pacman.6e/6f/6h/6j)
//   0x4000-0x43FF  video RAM (tile numbers)
//   0x4400-0x47FF  color RAM (per-tile color index, low 5 bits used)
//   0x4800-0x4BFF  unmapped ("pacman_read_nop": reads return a fixed 0xBF,
//                  writes ignored -- MAME's own comment calls the exact
//                  open-bus value "inconclusive" but 0xBF is what its
//                  `pacman_read_nop()` returns and no real game code
//                  depends on this range for anything but Ms. Pac-Man's
//                  own quirk, not the plain "pacman" set this targets)
//   0x4C00-0x4FEF  work RAM
//   0x4FF0-0x4FFF  sprite number/flip/color ("spriteram", 8 sprites x 2B)
//   0x5000-0x5007  mainlatch writes (see pacman_machine.h's field comments)
//   0x5000         IN0 read
//   0x5040-0x505F  Namco WSG voice register writes (see pacman_audio.*)
//   0x5040         IN1 read
//   0x5060-0x506F  sprite x/y ("spriteram2", 8 sprites x 2B)
//   0x5080         DSW1 read; writes here are a no-op on real hardware
//   0x50C0         DSW2 read (fixed 0xFF -- this hardware has no real
//                  second DIP bank, see pacman_machine.h); watchdog reset
//                  on write (not emulated -- no watchdog in this port)
//
// I/O address space (Z80 IN/OUT instructions, separate from the memory
// map above -- verified against MAME's pacman_state::writeport()):
//   OUT (0x00),A   sets the IM0 interrupt vector byte (see
//                  pacman_machine.h's interrupt_vector field comment).
//                  Nothing else in this port map is used by the "pacman"
//                  ROM set -- port_in is stubbed.
#include "pacman_ports.h"

static uint8_t pacman_read_byte(void *userdata, uint16_t addr) {
    pacman_system *sys = (pacman_system *)userdata;

    if (addr < 0x4000) return sys->rom[addr];
    if (addr < 0x4400) return sys->video_ram[addr - 0x4000];
    if (addr < 0x4800) return sys->color_ram[addr - 0x4400];
    if (addr < 0x4C00) return 0xBF; // unmapped -- see header comment
    if (addr < 0x4FF0) return sys->work_ram[addr - 0x4C00];
    if (addr < 0x5000) return sys->sprite_num[addr - 0x4FF0];
    if (addr < 0x5040) return sys->in0;
    if (addr < 0x5080) return sys->in1;
    if (addr < 0x50C0) return sys->dsw1;
    if (addr < 0x5100) return 0xFF; // DSW2 -- doesn't exist on this hardware
    return 0xFF; // nothing else is mapped on real hardware
}

static void pacman_write_byte(void *userdata, uint16_t addr, uint8_t data) {
    pacman_system *sys = (pacman_system *)userdata;

    if (addr < 0x4000) return; // ROM -- not writable
    if (addr < 0x4400) { sys->video_ram[addr - 0x4000] = data; return; }
    if (addr < 0x4800) { sys->color_ram[addr - 0x4400] = data; return; }
    if (addr < 0x4C00) return; // unmapped, nopw
    if (addr < 0x4FF0) { sys->work_ram[addr - 0x4C00] = data; return; }
    if (addr < 0x5000) { sys->sprite_num[addr - 0x4FF0] = data; return; }

    if (addr <= 0x5007) {
        // 74LS259 mainlatch: bit 0 of `data` sets/clears output line
        // (addr - 0x5000). Bits 2/4/5/6 are real-but-unwired on this
        // hardware (see pacman_machine.h) -- fall through and ignore.
        bool bit = (data & 0x01) != 0;
        switch (addr - 0x5000) {
        case 0: sys->interrupt_enable = bit; break;
        case 1: sys->sound_enable     = bit; break;
        case 3: sys->flip_screen      = bit; break;
        case 7: sys->coin_counter     = bit; break;
        default: break;
        }
        return;
    }
    // 0x5008-0x503F: unmapped (nopw region between the mainlatch and the
    // sound registers) -- must NOT fall into the 0x5040-0x505F check
    // below, or `addr - 0x5040` underflows into a wild array write.
    if (addr < 0x5040) return;
    if (addr <= 0x505F) { sys->sound_regs[addr - 0x5040] = data & 0x0F; return; }
    if (addr <= 0x506F) { sys->sprite_pos[addr - 0x5060] = data; return; }
    // 0x5070-0x50FF (nopw region, DSW1/watchdog write-side): no-op.
}

static uint8_t pacman_port_in(z80 *cpu, uint8_t port) {
    (void)cpu; (void)port;
    return 0xFF; // not used by the "pacman" ROM set -- see header comment
}

static void pacman_port_out(z80 *cpu, uint8_t port, uint8_t data) {
    pacman_system *sys = (pacman_system *)cpu->userdata;
    if (port == 0x00) sys->interrupt_vector = data;
}

void pacman_ports_wire(pacman_system *system) {
    system->cpu.read_byte  = pacman_read_byte;
    system->cpu.write_byte = pacman_write_byte;
    system->cpu.port_in    = pacman_port_in;
    system->cpu.port_out   = pacman_port_out;
    system->cpu.userdata   = system;
}
