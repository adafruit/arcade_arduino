// Donkey Kong Z80 bus wiring -- memory map plus the i8257 DMA controller
// that is the only path sprites take to the video hardware.
//
// Memory map, verified against MAME's dkong_state::dkong_map()
// (src/mame/nintendo/dkong.cpp):
//
//   0x0000-0x4FFF  ROM (the `dkong` set fills 0x0000-0x3FFF; 0x4000-0x4FFF
//                  is mapped as ROM but has no chip in this set)
//   0x6000-0x6BFF  work RAM
//   0x7000-0x73FF  sprite RAM ("sprite_ram") -- written by DMA, not by the CPU
//   0x7400-0x77FF  video RAM (tile numbers)
//   0x7800-0x780F  i8257 DMA controller registers
//   0x7C00         r: IN0   w: sound command latch (ls175.3d)
//   0x7C80         r: IN1   w: radarscp grid colour -- not this machine
//   0x7D00         r: IN2   w(0x7D00-0x7D07): sound signal latch (6H)
//   0x7D80         r: DSW0  w: sound CPU external interrupt
//   0x7D81         w: radarscp grid enable -- not this machine
//   0x7D82         w: flip screen
//   0x7D83         w: sprite bank
//   0x7D84         w: NMI mask
//   0x7D85         w: 8257 /DRQ0 + /DRQ1 -- runs the DMA (see dma_run())
//   0x7D86-0x7D87  w: palette bank (one bit per address, bit 0 of data)
//
// Everything above 0x7D87 is unmapped on this board. The `dkong` set has no
// I/O-space (IN/OUT) accesses at all -- unlike Pac-Man, which uses OUT (0),A
// to hand the Z80 its IM0 vector -- because this machine interrupts by NMI
// instead. port_in/port_out are stubs.
//
// The sound writes (0x7C00, 0x7D00-0x7D07, 0x7D80) all route into
// dkong_audio.cpp, which owns the 8035 sound CPU and the discrete-channel
// approximations. IN2 bit 6 reads back from that side too -- it is the
// sound CPU's status line, and is the only place the sound hardware is
// visible to the main CPU at all.
#include <string.h>
#include "dkong_ports.h"
#include "dkong_audio.h"

// --- i8257 DMA -----------------------------------------------------------
//
// From MAME's i8257_device (src/devices/machine/i8257.cpp) as wired by
// dkong_base() in dkong.cpp. This is deliberately NOT a general 8257 model;
// it implements exactly the configuration Donkey Kong uses, and the reasons
// are worth stating because a future machine reusing this file would need
// more:
//
//   - dkong_base() wires in_memr_cb/out_memw_cb to main memory, out_iow_cb<0>
//     to a one-byte latch (p8257_ctl_w) and in_ior_cb<1> to that same latch
//     (p8257_ctl_r). So channel 0 reads memory into the latch and channel 1
//     writes the latch back out to memory: the classic 8257 memory-to-memory
//     pairing, and in effect a block copy.
//   - p8257_drq_w() asserts DREQ0 and DREQ1 together and comments "transfer
//     occurs immediately". So the whole transfer is run synchronously right
//     here, at the instant of the 0x7D85 write, rather than being spread
//     over subsequent cycles. That is a simplification, but a faithful one
//     for this board: MAME does the same thing in effect by aborting the
//     timeslice.
//   - Mode bits: 1 = WRITE (I/O -> memory), 2 = READ (memory -> I/O), per
//     MODE_TRANSFER_WRITE/MODE_TRANSFER_READ in i8257.cpp. dkong_base() also
//     calls set_reverse_rw_mode(true), which swaps 1 and 2 as they are
//     WRITTEN by the game -- that swap is applied in dma_write_reg() below,
//     exactly where i8257.cpp applies it.
//   - The count register holds N-1 for an N-byte transfer, and terminal
//     count is reached when it reads 0 (i8257.cpp's advance()).

static uint32_t g_dma_transfers = 0, g_dma_bytes = 0;

void dkong_debug_take_dma_stats(uint32_t *out_transfers, uint32_t *out_bytes) {
    if (out_transfers) *out_transfers = g_dma_transfers;
    if (out_bytes)     *out_bytes     = g_dma_bytes;
    g_dma_transfers = 0;
    g_dma_bytes = 0;
}

static uint8_t dkong_read_byte(void *userdata, uint16_t addr);
static void dkong_write_byte(void *userdata, uint16_t addr, uint8_t data);

// Register writes at 0x7800-0x780F. Address and count registers are 16 bits
// written as two 8-bit halves, low then high, selected by an internal
// flip-flop (`msb`) that toggles on EVERY access to a channel register --
// including reads. Getting that toggle wrong desynchronises every
// subsequent write and produces a DMA that copies from a plausible-looking
// but wrong address.
static void dma_write_reg(dkong_system *sys, uint16_t offset, uint8_t data) {
    dkong_dma *d = &sys->dma;

    if (offset & 0x08) { // register 8: mode set
        d->enable = data;
        return;
    }

    int ch = (offset >> 1) & 0x03;
    if (offset & 0x01) { // word count
        if (d->msb) {
            d->count[ch] = (uint16_t)(((data & 0x3F) << 8) | (d->count[ch] & 0xFF));
            uint8_t mode = (uint8_t)(data >> 6);
            // set_reverse_rw_mode(true): swap READ and WRITE as written.
            if (mode) mode = (mode == 1) ? 2 : 1;
            d->mode[ch] = mode;
        } else {
            d->count[ch] = (uint16_t)((d->count[ch] & 0xFF00) | data);
        }
    } else {             // address
        if (d->msb) d->address[ch] = (uint16_t)((data << 8) | (d->address[ch] & 0xFF));
        else        d->address[ch] = (uint16_t)((d->address[ch] & 0xFF00) | data);
    }
    d->msb = !d->msb;
}

static uint8_t dma_read_reg(dkong_system *sys, uint16_t offset) {
    dkong_dma *d = &sys->dma;
    if (offset & 0x08) return 0; // status register: no TC bits modelled

    int ch = (offset >> 1) & 0x03;
    uint8_t data;
    if (offset & 0x01) {
        data = d->msb ? (uint8_t)(d->count[ch] >> 8) : (uint8_t)(d->count[ch] & 0xFF);
        if (d->msb) {
            uint8_t m = d->mode[ch];
            data |= m ? ((m == 1) ? 0x80 : 0x40) : 0; // reverse_rw read-back, per i8257.cpp
        }
    } else {
        data = d->msb ? (uint8_t)(d->address[ch] >> 8) : (uint8_t)(d->address[ch] & 0xFF);
    }
    d->msb = !d->msb;
    return data;
}

// Runs the whole transfer, triggered by a 0x7D85 write with bit 0 set.
// Channel 0 (mode READ) reads memory into the latch; channel 1 (mode WRITE)
// writes the latch to memory. Both advance and both count down; the
// transfer ends when either reaches terminal count.
//
// The byte-at-a-time round trip through `latch` is not ceremony -- it is
// what the hardware does, and keeping it makes the mode handling above
// meaningful rather than decorative.
static void dma_run(dkong_system *sys) {
    dkong_dma *d = &sys->dma;

    // Both channels must be enabled and correctly programmed; anything else
    // is a transfer this board never asks for, and running it would be
    // guesswork rather than emulation.
    if ((d->enable & 0x03) != 0x03) return;
    if (d->mode[0] != 2 || d->mode[1] != 1) return;

    uint32_t moved = 0;
    // Bounded by the 14-bit count width so a mis-programmed transfer cannot
    // spin forever on device -- 0x4000 iterations is already far more than
    // this game ever asks for (its sprite block is 0x180 bytes).
    for (uint32_t guard = 0; guard < 0x4000u; guard++) {
        d->latch = dkong_read_byte(sys, d->address[0]);
        dkong_write_byte(sys, d->address[1], d->latch);
        moved++;

        bool tc = (d->count[0] == 0) || (d->count[1] == 0);
        d->address[0]++; d->address[1]++;
        d->count[0] = (uint16_t)((d->count[0] - 1) & 0x3FFF);
        d->count[1] = (uint16_t)((d->count[1] - 1) & 0x3FFF);
        if (tc) break;
    }

    g_dma_transfers++;
    g_dma_bytes += moved;
}

// --- Z80 bus -------------------------------------------------------------

static uint8_t dkong_read_byte(void *userdata, uint16_t addr) {
    dkong_system *sys = (dkong_system *)userdata;

    if (addr < DKONG_ROM_SIZE)                 return sys->rom[addr];
    if (addr >= 0x6000 && addr <= 0x6BFF)      return sys->work_ram[addr - 0x6000];
    if (addr >= 0x7000 && addr <= 0x73FF)      return sys->sprite_ram[addr - 0x7000];
    if (addr >= 0x7400 && addr <= 0x77FF)      return sys->video_ram[addr - 0x7400];
    if (addr >= 0x7800 && addr <= 0x780F)      return dma_read_reg(sys, addr - 0x7800);

    // The four input/DIP reads. Each is decoded at a single address in
    // MAME's map; this board does not mirror them, and no ROM code here
    // relies on aliasing (unlike Space Invaders' self-test -- see
    // ArcadeCPU_i8080's mirror_2000_at_4000 comment).
    if (addr == 0x7C00) return sys->in0;
    if (addr == 0x7C80) return sys->in1;
    // IN2 bit 6 is the sound CPU's status line, live from the 8035's P2
    // latch -- dkong_input.cpp fills in every other bit but cannot know
    // this one. Before sound existed it was hard-coded to the idle value.
    if (addr == 0x7D00) return (uint8_t)((sys->in2 & ~0x40) | dkong_audio_status_r());
    if (addr == 0x7D80) return sys->dsw0;

    return 0xFF; // nothing else is mapped
}

static void dkong_write_byte(void *userdata, uint16_t addr, uint8_t data) {
    dkong_system *sys = (dkong_system *)userdata;

    if (addr < DKONG_ROM_SIZE) return; // ROM -- not writable
    if (addr >= 0x6000 && addr <= 0x6BFF) { sys->work_ram[addr - 0x6000] = data; return; }
    if (addr >= 0x7000 && addr <= 0x73FF) { sys->sprite_ram[addr - 0x7000] = data; return; }
    if (addr >= 0x7400 && addr <= 0x77FF) { sys->video_ram[addr - 0x7400] = data; return; }
    if (addr >= 0x7800 && addr <= 0x780F) { dma_write_reg(sys, addr - 0x7800, data); return; }

    if (addr == 0x7C00) { dkong_audio_command_w(data); return; } // sound command latch (ls175.3d)
    if (addr == 0x7C80) return; // radarscp grid colour -- not this machine

    // Signal latch (6H): one bit per address, from bit 0 of the data. Bits
    // 0/1/2 and 6/7 drive the discrete channels, bit 3 is readable by the
    // 8035 on P2, and bits 4/5 are its T1/T0 test inputs.
    if (addr >= 0x7D00 && addr <= 0x7D07) {
        dkong_audio_signal_w((uint8_t)(addr - 0x7D00), data);
        return;
    }

    switch (addr) {
    case 0x7D80: dkong_audio_irq_w(data); return;         // sound CPU external interrupt
    case 0x7D81: return;                                  // radarscp grid enable -- not this machine
    case 0x7D82: sys->flip_screen = (data & 0x01) != 0; return;
    case 0x7D83: sys->sprite_bank = (uint8_t)(data & 0x01); return;
    case 0x7D84: sys->nmi_mask    = (data & 0x01) != 0; return;
    case 0x7D85:
        // /DRQ0 + /DRQ1. MAME's p8257_drq_w() sets both from bit 0 and
        // comments that the transfer occurs immediately.
        if (data & 0x01) dma_run(sys);
        return;
    case 0x7D86:
    case 0x7D87: {
        // dkong_palettebank_w(): `offset` selects WHICH BIT, and bit 0 of
        // the data says whether to set or clear it. It is not a plain
        // two-bit write, and treating it as one gives subtly wrong colours
        // in exactly the places the game changes palette.
        uint8_t bit = (uint8_t)(1u << (addr - 0x7D86));
        if (data & 0x01) sys->palette_bank |= bit;
        else             sys->palette_bank &= (uint8_t)~bit;
        return;
    }
    default: return;
    }
}

static uint8_t dkong_port_in(z80 *cpu, uint8_t port) {
    (void)cpu; (void)port;
    return 0xFF; // no I/O-space reads on this board -- see header comment
}

static void dkong_port_out(z80 *cpu, uint8_t port, uint8_t data) {
    (void)cpu; (void)port; (void)data; // no I/O-space writes on this board
}

void dkong_ports_wire(dkong_system *system) {
    system->cpu.read_byte  = dkong_read_byte;
    system->cpu.write_byte = dkong_write_byte;
    system->cpu.port_in    = dkong_port_in;
    system->cpu.port_out   = dkong_port_out;
    system->cpu.userdata   = system;
}
