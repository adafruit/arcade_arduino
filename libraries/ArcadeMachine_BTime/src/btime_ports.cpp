// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Burger Time memory maps, I/O decode, and the DECO CPU-7 opcode
// descrambler.
//
// MAIN CPU MAP -- transcribed from btime_state::btime_map() in
// src/mame/dataeast/btime.cpp:
//
//   0000-07FF  RAM (2K)
//   0C00-0C0F  palette RAM (16 bytes; reads return the RAM, writes also
//              rebuild the RGB565 lookup -- see btime_video.cpp)
//   1000-13FF  video RAM  (char codes, and 8 sprites interleaved)
//   1400-17FF  colour RAM (char code bits 8-9)
//   1800-1BFF  video RAM  with X and Y SWAPPED (read+write)
//   1C00-1FFF  colour RAM with X and Y SWAPPED (read+write)
//   4000       r: P1      w: ignored (MAME: .nopw())
//   4001       r: P2
//   4002       r: SYSTEM   w: video control (bit 0 = flip screen)
//   4003       r: DSW1     w: sound latch (and asserts the sound CPU's IRQ)
//   4004       r: DSW2     w: bnj_scroll[0] (background enable/bank/scroll)
//   B000-FFFF  ROM
//
// The swapped mirrors are real hardware -- two address decoders onto the
// same RAM, one with the coordinates exchanged. The driver header explains
// what they are for: "There are two ports to the video and color RAMs, one
// normal access, and one with X and Y coordinates swapped. The sprite RAM
// occupies the first row of the swapped area, so it appears in the regular
// video RAM as the first column of on the left side." The program uses
// whichever decoder suits the shape it is drawing.
//
// SOUND CPU MAP -- from btime_state::audio_map(), and note how enormous the
// decode windows are (one address line each). A port that only decodes the
// exact base address silently drops most register writes:
//
//   0000-1FFF  RAM (0x400, mirrored)
//   2000-3FFF  w: AY1 data      4000-5FFF  w: AY1 address
//   6000-7FFF  w: AY2 data      8000-9FFF  w: AY2 address
//   A000-BFFF  r: sound latch -- THE READ CLEARS THE IRQ
//   C000-DFFF  w: audio NMI enable (bit 0)
//   E000-EFFF  ROM, mirrored at F000-FFFF (so the vectors resolve)
#include <string.h>
#include "btime_ports.h"
#include "btime_video.h"
#include "btime_audio.h"

// Diagnostic counters -- see btime_machine.h's btime_debug_take_counters().
static uint32_t g_vblank_reads;
static uint32_t g_opcode_swaps;
static uint32_t g_mirror_reads;
static uint32_t g_latch_writes;
static uint32_t g_system_reads;

// Swaps X and Y within a 32x32 page, exactly as MAME's
// btime_mirrorvideoram_r/w() and btime_mirrorcolorram_r/w() do:
//     int const x = offset / 32;  int const y = offset % 32;
//     offset = 32 * y + x;
static inline uint16_t swap_xy(uint16_t offset) {
    uint16_t x = offset / 32u;
    uint16_t y = offset % 32u;
    return (uint16_t)(32u * y + x);
}

// --- main CPU ------------------------------------------------------------

static uint8_t main_read(void *userdata, uint16_t addr) {
    btime_system *s = (btime_system *)userdata;

    if (addr < 0x0800) return s->ram[addr];
    if (addr >= 0x0C00 && addr <= 0x0C0F) return s->paletteram[addr - 0x0C00];
    if (addr >= 0x1000 && addr <= 0x13FF) return s->videoram[addr - 0x1000];
    if (addr >= 0x1400 && addr <= 0x17FF) return s->colorram[addr - 0x1400];
    if (addr >= 0x1800 && addr <= 0x1BFF) {
        g_mirror_reads++;
        return s->videoram[swap_xy(addr - 0x1800)];
    }
    if (addr >= 0x1C00 && addr <= 0x1FFF) {
        g_mirror_reads++;
        return s->colorram[swap_xy(addr - 0x1C00)];
    }
    if (addr >= 0x4000 && addr <= 0x4004) {
        switch (addr) {
        case 0x4000: return s->p1;
        case 0x4001: return s->p2;
        case 0x4002: g_system_reads++; return s->system_in;
        case 0x4003:
            // DSW1 -- except bit 7, which is NOT a DIP switch. From
            // INPUT_PORTS_START( btime ):
            //   PORT_BIT( 0x80, IP_ACTIVE_HIGH, IPT_CUSTOM )
            //     PORT_READ_LINE_DEVICE_MEMBER("screen", ...vblank)
            //     // Schematics show this is connected to DIP SW2.8
            // With no vblank interrupt on this board, this bit is the ONLY
            // way the program knows where the beam is. Returning a constant
            // here hangs the game; returning it inverted makes it draw
            // during the visible area.
            g_vblank_reads++;
            return (uint8_t)((s->dsw1 & 0x7F) | (s->vblank ? 0x80 : 0x00));
        case 0x4004: return s->dsw2;
        }
    }
    if (addr >= BTIME_ROM_BASE) return s->rom[addr - BTIME_ROM_BASE];

    // Everything else is unmapped. MAME's btime_map() leaves these holes
    // with no handler, so a read returns whatever that address space's
    // unmapped value is; 0 is this port's assumption, and the port plan
    // lists it as an open question (DEVNOTES.md #24 is the standing warning
    // that open-bus reads can be load-bearing on this kind of board).
    return 0x00;
}

static void main_write(void *userdata, uint16_t addr, uint8_t val) {
    btime_system *s = (btime_system *)userdata;

    // THE DECO CPU-7's ENTIRE STATE. MAME's mi_decrypt::write() sets
    // m_had_written on EVERY write the CPU makes, whatever the address, and
    // read_sync() tests and clears it. Set it first and unconditionally --
    // an early return further down that skipped this would silently break
    // the descrambler for whatever wrote there.
    s->had_written = true;

    if (addr < 0x0800) { s->ram[addr] = val; return; }
    if (addr >= 0x0C00 && addr <= 0x0C0F) {
        s->paletteram[addr - 0x0C00] = val;
        btime_video_palette_write(addr - 0x0C00, val);
        return;
    }
    if (addr >= 0x1000 && addr <= 0x13FF) { s->videoram[addr - 0x1000] = val; return; }
    if (addr >= 0x1400 && addr <= 0x17FF) { s->colorram[addr - 0x1400] = val; return; }
    if (addr >= 0x1800 && addr <= 0x1BFF) {
        s->videoram[swap_xy(addr - 0x1800)] = val;
        return;
    }
    if (addr >= 0x1C00 && addr <= 0x1FFF) {
        s->colorram[swap_xy(addr - 0x1C00)] = val;
        return;
    }
    switch (addr) {
    case 0x4000: return;                 // MAME: .nopw()
    case 0x4002:                         // btime_video_control_w()
        s->flip_screen = (val & 0x01) != 0;
        return;
    case 0x4003:                         // generic_latch_8::write
        // Writing the latch asserts the sound CPU's IRQ line, via
        // m_soundlatch->data_pending_callback().set_inputline(m_audiocpu, 0).
        // It is a LEVEL: gen_latch.cpp's read() clears it again (see
        // audio_read below), not the act of taking the interrupt.
        s->soundlatch = val;
        s->sound_irq  = true;
        g_latch_writes++;
        return;
    case 0x4004:                         // bnj_scroll_w<0>()
        s->bnj_scroll0 = val;
        return;
    default: return;                     // unmapped, including ROM space
    }
}

// The DECO CPU-7. Ported from deco_cpu7_device::mi_decrypt::read_sync()
// (src/mame/dataeast/decocpu7.cpp:28):
//
//     uint8_t res = m_cprogram.read_byte(adr);
//     if (m_had_written) {
//         m_had_written = false;
//         if ((adr & 0x0104) == 0x0104)
//             res = bitswap<8>(res, 6,5,3,4,2,7,1,0);
//     }
//     return res;
//
// THREE THINGS THAT ARE EASY TO GET WRONG HERE, all of them visible in
// those six lines once you know to look:
//
//  - The flag means "has any write happened since the last opcode fetch",
//    NOT "was the previous bus cycle a write". mi_decrypt overrides only
//    read_sync and write, so ordinary operand and data reads pass straight
//    through the base class and do NOT clear it. The observable consequence:
//    after a JSR -- whose last bus cycle is a READ of the target address's
//    high byte -- the flag is still set when the next opcode is fetched.
//    That is exactly why this is implementable in an instruction-level core
//    at all.
//  - The flag is cleared whenever it was set, whether or not the address
//    test passes. A fetch at a non-matching address still consumes it.
//  - INTERRUPTS ARM IT. Taking an IRQ or NMI pushes PCH, PCL and P, which
//    are three writes through this same callback, so the handler's FIRST
//    opcode is descrambled if its address matches. ArcadeCPU_M6502's
//    interrupt() pushes through write_byte, so this happens naturally --
//    but it means a handler at a matching address is genuinely affected,
//    and "my interrupt handler executes garbage" would otherwise be a
//    baffling symptom.
static uint8_t main_read_opcode(void *userdata, uint16_t addr) {
    btime_system *s = (btime_system *)userdata;
    uint8_t res = main_read(userdata, addr);

    if (s->had_written) {
        s->had_written = false;
        if ((addr & 0x0104) == 0x0104) {
            // bitswap<8>(res, 6,5,3,4,2,7,1,0): result bit 7 takes source
            // bit 6, bit 6 takes 5, bit 5 takes 3, bit 4 takes 4, bit 3
            // takes 2, bit 2 takes 7, bit 1 takes 1, bit 0 takes 0.
            res = (uint8_t)(((res >> 6) & 1) << 7 |
                            ((res >> 5) & 1) << 6 |
                            ((res >> 3) & 1) << 5 |
                            ((res >> 4) & 1) << 4 |
                            ((res >> 2) & 1) << 3 |
                            ((res >> 7) & 1) << 2 |
                            ((res >> 1) & 1) << 1 |
                            ((res >> 0) & 1) << 0);
            g_opcode_swaps++;
        }
    }
    return res;
}

// --- sound CPU -----------------------------------------------------------

static uint8_t audio_read(void *userdata, uint16_t addr) {
    btime_system *s = (btime_system *)userdata;

    if (addr < 0x2000) return s->audio_ram[addr & 0x03FF]; // .mirror(0x1c00)
    if (addr >= 0xA000 && addr <= 0xBFFF) {
        // generic_latch_8_device::read(): with no separate acknowledge
        // configured -- and btime configures none -- the READ is what
        // clears the pending flag:
        //     if (!has_separate_acknowledge() && !side_effects_disabled())
        //         set_latch_written(false);
        // So the sound CPU dismisses its own IRQ by fetching the command.
        s->sound_irq = false;
        return s->soundlatch;
    }
    if (addr >= 0xE000) return s->audio_rom[addr & 0x0FFF]; // .mirror(0x1000)
    return 0x00;
}

static void audio_write(void *userdata, uint16_t addr, uint8_t val) {
    btime_system *s = (btime_system *)userdata;
    (void)s;

    if (addr < 0x2000) { s->audio_ram[addr & 0x03FF] = val; return; }
    if (addr <= 0x3FFF) { btime_audio_data_w(0, val);    return; } // 2000-3FFF
    if (addr <= 0x5FFF) { btime_audio_address_w(0, val); return; } // 4000-5FFF
    if (addr <= 0x7FFF) { btime_audio_data_w(1, val);    return; } // 6000-7FFF
    if (addr <= 0x9FFF) { btime_audio_address_w(1, val); return; } // 8000-9FFF
    if (addr >= 0xC000 && addr <= 0xDFFF) {
        // btime_state::audio_nmi_enable_w(). btime is AUDIO_ENABLE_DIRECT
        // (init_btime() sets m_audio_nmi_enable_type), so the enable is bit
        // 0 written HERE -- not the first AY's port A, which is the route
        // Zoar and Lock'n'Chase use. It starts clear: machine_reset() does
        // m_audionmi->in_w<0>(0), and the ROM turns it on.
        s->audio_nmi_en = (val & 0x01) != 0;
        return;
    }
}

// --- wiring --------------------------------------------------------------

void btime_ports_wire(btime_system *system) {
    system->cpu.userdata    = system;
    system->cpu.read_byte   = &main_read;
    system->cpu.write_byte  = &main_write;
    system->cpu.read_opcode = &main_read_opcode; // the CPU-7; see above

    system->audiocpu.userdata    = system;
    system->audiocpu.read_byte   = &audio_read;
    system->audiocpu.write_byte  = &audio_write;
    system->audiocpu.read_opcode = NULL; // a plain 6502: fetches are reads
}

void btime_ports_reset_cpus(btime_system *system) {
    // m6502_gen_res() reads the reset vector out of 0xFFFC/0xFFFD through
    // read_byte, so this is only meaningful once the ROM is in place.
    m6502_gen_res(&system->cpu);
    m6502_gen_res(&system->audiocpu);

    // A reset pushes nothing, but gen_res() does go through interrupt(),
    // and this port's write callback is the only thing that sets
    // had_written -- so make the post-reset state explicit rather than
    // whatever the reset sequence happened to leave.
    system->had_written = false;
}

void btime_ports_coin_irq(btime_system *system) {
    // INPUT_CHANGED_MEMBER(btime_state::coin_inserted_irq_hi):
    //     if (newval) m_maincpu->set_input_line(0, HOLD_LINE);
    // HOLD_LINE means the line is asserted and cleared automatically when
    // the CPU acknowledges it, so a one-shot pending flag is the faithful
    // model -- not a level the ROM has to dismiss.
    system->coin_irq_pending = true;
}

// Reports and resets the three counters this file owns. The public
// btime_debug_take_counters() lives in btime_machine.cpp, which adds the
// illegal-opcode count -- that one belongs to the CPU cores, not here.
void btime_ports_take_counters(uint32_t *out_vblank_reads,
                               uint32_t *out_opcode_swaps,
                               uint32_t *out_mirror_reads,
                               uint32_t *out_latch_writes,
                               uint32_t *out_system_reads) {
    if (out_vblank_reads) *out_vblank_reads = g_vblank_reads;
    if (out_opcode_swaps) *out_opcode_swaps = g_opcode_swaps;
    if (out_mirror_reads) *out_mirror_reads = g_mirror_reads;
    if (out_latch_writes) *out_latch_writes = g_latch_writes;
    if (out_system_reads) *out_system_reads = g_system_reads;
    g_vblank_reads = 0;
    g_opcode_swaps = 0;
    g_mirror_reads = 0;
    g_latch_writes = 0;
    g_system_reads = 0;
}
