// Ms. Pac-Man Z80 bus wiring -- the banked address space, the aux board's
// address-triggered bank switching, and the one real I/O-space write.
//
// Verified against MAME's pacman_state::mspacman_map()
// (src/mame/pacman/pacman.cpp). That map is NOT pacman_map() plus extras --
// it is its own map, and the differences from ArcadeMachine_Pacman's
// mspacman_ports.cpp sibling are the entire reason this library exists:
//
//   1. ROM IS BANKED over the whole 64K:
//        map(0x0000, 0xffff).bankr("bank1");
//      with two 64K entries configured by init_mspacman() -- [0] the plain
//      Pac-Man code, [1] the decrypted Ms. Pac-Man code. Only 0x0000-0x3FFF
//      and 0x8000-0xBFFF are actually ROM; the rest is overlaid below.
//
//   2. RAM/IO IS MIRRORED with a 0xA000 mask. Every RAM and I/O entry in
//      mspacman_map() carries `.mirror(0xa000)` (or 0xaf00/0xaf38/0xaf3f for
//      the I/O block), where pacman_map()'s equivalents use plain 0x8000-less
//      ranges. A mirror mask means those address bits are simply not decoded,
//      so 0x4000-0x7FFF also answers at 0xC000-0xFFFF. This is NOT the
//      optional incomplete-decoding aliasing ArcadeMachine_Pacman
//      deliberately skips: with ROM now occupying 0x8000-0xBFFF, the high
//      quarter of the address space is where this game's RAM actually lives
//      from the CPU's point of view, so the mask is load-bearing.
//
//   3. EIGHT ADDRESS RANGES FLIP THE BANK ON ANY ACCESS -- read or write,
//      with the read still returning a byte. MAME overlays them last:
//        0x0038-0x003F  disable      0x03B0-0x03B7  disable
//        0x1600-0x1607  disable      0x2120-0x2127  disable
//        0x3FF0-0x3FF7  disable      0x3FF8-0x3FFF  ENABLE
//        0x8000-0x8007  disable      0x97F0-0x97F7  disable
//      "disable" selects the plain Pac-Man bank, "enable" the decrypted one.
//      Note the asymmetry, which is real: one enable range, seven disable.
//      Note also that the returned byte comes from the bank being switched
//      TO, not the one that was live on entry -- mspacman_enable_decode_r()
//      returns `base()[offset + Delta + 0x10000]` (the decrypted bank) after
//      setting entry 1, and mspacman_disable_decode_r() returns
//      `base()[offset + Delta]` (the plain bank) after setting entry 0. That
//      is what the code below does by flipping `bank` before reading it.
//      These overlays carry no `.mirror()`, so they answer at their literal
//      addresses only.
//
// Everything else -- the RAM/IO layout itself, the mainlatch, the WSG
// registers, IN0/IN1/DSW1/DSW2, the IM0 vector write -- is identical to
// Pac-Man's and is documented in ArcadeMachine_Pacman's pacman_ports.cpp:
//
//   0x4000-0x43FF  video RAM (tile numbers)
//   0x4400-0x47FF  color RAM (per-tile color index, low 5 bits used)
//   0x4800-0x4BFF  unmapped ("pacman_read_nop": reads return 0xBF)
//   0x4C00-0x4FEF  work RAM
//   0x4FF0-0x4FFF  sprite number/flip/color ("spriteram", 8 sprites x 2B)
//   0x5000-0x5007  mainlatch writes (see mspacman_machine.h's field comments)
//   0x5000         IN0 read
//   0x5040-0x505F  Namco WSG voice register writes (see mspacman_audio.*)
//   0x5040         IN1 read
//   0x5060-0x506F  sprite x/y ("spriteram2", 8 sprites x 2B)
//   0x5080         DSW1 read; writes here are a no-op on real hardware
//   0x50C0         DSW2 read (fixed 0xFF -- no second DIP bank on this
//                  hardware); watchdog reset on write (not emulated)
//
// One Pac-Man-era note that matters MORE here: MAME's pacman_read_nop()
// comment says "Ms Pacman reads bytes in sequence until it hits a 0 for a
// delimiter, including empty areas". The 0xBF this returns for 0x4800-0x4BFF
// is therefore read by this game's own code, not just by Pac-Man's; MAME
// calls the true open-bus value "inconclusive" but 0xBF is what it returns
// and what this port matches.
//
// I/O address space (Z80 IN/OUT, separate from the memory map above):
//   OUT (0x00),A   sets the IM0 interrupt vector byte (see
//                  mspacman_machine.h's interrupt_vector field comment).
#include "mspacman_ports.h"

// True if `addr` is inside one of the eight bank-trigger ranges; sets
// *out_bank to the bank that access selects. Each range is 8 bytes aligned
// to an 8-byte boundary, so a single masked compare covers each one.
//
// PERFORMANCE NOTE, since this sits on the CPU's hot path: this is up to
// eight compares, and it is reached on every ROM access -- which includes
// every instruction fetch. Callers below deliberately test the RAM/IO block
// FIRST and only consult this for ROM addresses; that is safe because all
// eight trigger bases have bit 14 clear (0x0038, 0x03B0, 0x1600, 0x2120,
// 0x3FF0, 0x3FF8, 0x8000, 0x97F0), so none of them can alias into RAM/IO,
// and it keeps data accesses to work/video/color RAM off this path
// entirely. If this game ever turns out to be short of frame budget, this
// function is the first place to look -- but measure before rewriting it
// (a const lookup table would be the obvious "fix" and would be a
// PESSIMISATION here: this library is flash-resident, so a table read on
// every memory access is an XIP fetch in the hottest loop in the program,
// exactly the trap DEVNOTES.md problems #17 and #25 are about).
static inline bool bank_trigger(uint16_t addr, uint8_t *out_bank) {
    switch (addr & 0xFFF8u) {
    case 0x3FF8u: *out_bank = MSPACMAN_BANK_DECRYPTED; return true; // the only enable range
    case 0x3FF0u:
    case 0x0038u:
    case 0x03B0u:
    case 0x1600u:
    case 0x2120u:
    case 0x8000u:
    case 0x97F0u: *out_bank = MSPACMAN_BANK_PLAIN;     return true;
    default:      return false;
    }
}

// Strips the mirror bits the RAM/IO block does not decode (see note 2 in the
// header comment): bit 15 and bit 13 are ignored, so 0x4000-0x5FFF,
// 0x6000-0x7FFF, 0xC000-0xDFFF and 0xE000-0xFFFF all land on the same
// storage. Only ever applied to addresses that already decoded as RAM/IO.
#define MIRROR_MASK 0x5FFFu

// True when `addr` falls in the RAM/IO block rather than ROM. Bit 14 alone
// decides it: set means 0x4000-0x7FFF or 0xC000-0xFFFF (RAM/IO), clear means
// 0x0000-0x3FFF or 0x8000-0xBFFF (banked ROM).
static inline bool is_ram_io(uint16_t addr) { return (addr & 0x4000u) != 0; }

static uint8_t mspacman_read_byte(void *userdata, uint16_t addr) {
    mspacman_system *sys = (mspacman_system *)userdata;

    if (!is_ram_io(addr)) {
        // ROM. Bank triggers are overlaid on top of it and are therefore
        // checked first, exactly as MAME installs them last (later installs
        // win over the .bankr() covering the whole space).
        uint8_t new_bank;
        if (bank_trigger(addr, &new_bank)) {
            sys->bank = new_bank;
            return sys->rom[new_bank][addr]; // the bank switched TO -- see header
        }
        return sys->rom[sys->bank][addr];
    }

    uint16_t a = addr & MIRROR_MASK;
    if (a < 0x4400) return sys->video_ram[a - 0x4000];
    if (a < 0x4800) return sys->color_ram[a - 0x4400];
    if (a < 0x4C00) return 0xBF; // unmapped -- see header comment
    if (a < 0x4FF0) return sys->work_ram[a - 0x4C00];
    if (a < 0x5000) return sys->sprite_num[a - 0x4FF0];
    if (a < 0x5040) return sys->in0;
    if (a < 0x5080) return sys->in1;
    if (a < 0x50C0) return sys->dsw1;
    if (a < 0x5100) return 0xFF; // DSW2 -- doesn't exist on this hardware
    return 0xFF; // nothing else is mapped on real hardware
}

static void mspacman_write_byte(void *userdata, uint16_t addr, uint8_t data) {
    mspacman_system *sys = (mspacman_system *)userdata;

    if (!is_ram_io(addr)) {
        // ROM -- not writable, but writes still flip the bank: MAME maps
        // each trigger range .rw(), with mspacman_enable_decode_w()/
        // mspacman_disable_decode_w() ignoring the data byte entirely and
        // only calling set_entry().
        uint8_t new_bank;
        if (bank_trigger(addr, &new_bank)) sys->bank = new_bank;
        return;
    }

    uint16_t a = addr & MIRROR_MASK;
    if (a < 0x4400) { sys->video_ram[a - 0x4000] = data; return; }
    if (a < 0x4800) { sys->color_ram[a - 0x4400] = data; return; }
    if (a < 0x4C00) return; // unmapped, nopw
    if (a < 0x4FF0) { sys->work_ram[a - 0x4C00] = data; return; }
    if (a < 0x5000) { sys->sprite_num[a - 0x4FF0] = data; return; }

    if (a <= 0x5007) {
        // 74LS259 mainlatch: bit 0 of `data` sets/clears output line
        // (a - 0x5000). Bit 6 is wired on THIS board and not on plain
        // Pac-Man's -- see mspacman_machine.h's coin_lockout comment.
        bool bit = (data & 0x01) != 0;
        switch (a - 0x5000) {
        case 0: sys->interrupt_enable = bit; break;
        case 1: sys->sound_enable     = bit; break;
        case 3: sys->flip_screen      = bit; break;
        case 6: sys->coin_lockout     = bit; break;
        case 7: sys->coin_counter     = bit; break;
        default: break;
        }
        return;
    }
    // 0x5008-0x503F: unmapped (nopw region between the mainlatch and the
    // sound registers) -- must NOT fall into the 0x5040-0x505F check
    // below, or `a - 0x5040` underflows into a wild array write.
    if (a < 0x5040) return;
    if (a <= 0x505F) { sys->sound_regs[a - 0x5040] = data & 0x0F; return; }
    if (a <= 0x506F) { sys->sprite_pos[a - 0x5060] = data; return; }
    // 0x5070-0x50FF (nopw region, DSW1/watchdog write-side): no-op.
}

static uint8_t mspacman_port_in(z80 *cpu, uint8_t port) {
    (void)cpu; (void)port;
    return 0xFF; // not used by the "mspacman" ROM set -- see header comment
}

static void mspacman_port_out(z80 *cpu, uint8_t port, uint8_t data) {
    mspacman_system *sys = (mspacman_system *)cpu->userdata;
    if (port == 0x00) sys->interrupt_vector = data;
}

void mspacman_ports_wire(mspacman_system *system) {
    system->cpu.read_byte  = mspacman_read_byte;
    system->cpu.write_byte = mspacman_write_byte;
    system->cpu.port_in    = mspacman_port_in;
    system->cpu.port_out   = mspacman_port_out;
    system->cpu.userdata   = system;
}
