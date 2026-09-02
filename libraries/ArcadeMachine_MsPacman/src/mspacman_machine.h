// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// ArcadeMachine_MsPacman: top-level Ms. Pac-Man machine state + lifecycle.
//
// Ms. Pac-Man is not its own PCB. It is a stock Pac-Man board with a
// daughterboard ("the Ms. Pac-Man auxiliary board") piggybacked onto the
// Z80, carrying three extra ROMs (u5/u6/u7) and an address decoder. Video,
// audio, input, the PROMs, the tile/sprite decode and the frame timing are
// all bit-identical to Pac-Man's -- this library is a sibling of
// ArcadeMachine_Pacman, and every file except mspacman_ports.cpp and
// mspacman_assets.cpp is a near-verbatim copy of its Pac-Man counterpart.
//
// EVERYTHING Ms. Pac-Man-specific lives in exactly two places:
//   - mspacman_assets.cpp -- building the decrypted ROM bank out of u5/u6/u7
//     (address- and data-line bitswaps) and applying the 40 eight-byte
//     patches, per MAME's pacman_state::init_mspacman() and
//     mspacman_install_patches().
//   - mspacman_ports.cpp  -- the banked address space and the eight address
//     ranges whose mere access flips the bank, per MAME's
//     pacman_state::mspacman_map().
//
// Every hardware fact used across this library's files (memory map, I/O map,
// interrupt scheme, tile/sprite decode, palette decode, WSG sound registers,
// and the aux-board banking/decode above) was verified directly against
// MAME's own `pacman` driver source (src/mame/pacman/pacman.cpp, pacman_v.cpp,
// devices/sound/namco.cpp/.h, upstream project mamedev/mame) rather than
// inferred from memory. Each file below cites the specific MAME construct
// its facts came from.
//
// Board-agnostic: talks only to ArcadeHAL and ArcadeCPU_Z80, never to a
// specific board's libraries.
#ifndef MSPACMAN_MACHINE_H
#define MSPACMAN_MACHINE_H

#include <stdint.h>
#include <stdbool.h>
#include "z80.h"

#ifdef __cplusplus
extern "C" {
#endif

// Pac-Man's raw hardware framebuffer resolution, BEFORE the cabinet's
// physical 90-degree mount -- same role INVADERS_GAME_WIDTH/HEIGHT play in
// invaders_machine.h. Verified via MAME's pacman() machine_config:
// m_screen->set_raw(18.432_MHz_XTAL/3, 384, 0, 288, 264, 0, 224) -- visible
// area is 288 horizontal x 224 vertical (288 = 36 tile columns x 8px,
// 224 = 28 tile rows x 8px, matching the 36x28 tilemap VIDEO_START creates).
#define MSPACMAN_GAME_WIDTH  288
#define MSPACMAN_GAME_HEIGHT 224

// Memory region sizes, drawn directly from MAME's mspacman_map() address map
// (src/mame/pacman/pacman.cpp) -- see mspacman_ports.cpp's header comment for
// the exact address ranges each one backs.
//
// ROM is BANKED, which is the whole point of the aux board. MAME's
// mspacman_map() opens with `map(0x0000, 0xffff).bankr("bank1")` over a
// 0x20000-byte "maincpu" region: a low 64K holding the plain Pac-Man code
// and a high 64K holding the decrypted Ms. Pac-Man code, with
// init_mspacman() calling `configure_entries(0, 2, &ROM[0x00000], 0x10000)`.
// Only two windows of each 64K bank are ever ROM -- 0x0000-0x3FFF and
// 0x8000-0xBFFF -- because 0x4000-0x7FFF (and its 0xC000 mirror) is the
// RAM/IO block. We therefore store 0xC000 per bank rather than MAME's full
// 0x10000, which costs 32KB of the 96KB total but keeps every index in
// mspacman_assets.cpp's decode identical to the MAME source it is
// transcribed from. Getting that transcription wrong is the single easiest
// way to break this port, so readability there is worth the SRAM here.
#define MSPACMAN_ROM_BANK_SIZE   0xC000 // 0x0000-0xBFFF of one bank (0x4000-0x7FFF unused)
#define MSPACMAN_ROM_BANKS       2      // [0] plain Pac-Man, [1] decrypted Ms. Pac-Man
#define MSPACMAN_BANK_PLAIN      0
#define MSPACMAN_BANK_DECRYPTED  1
#define MSPACMAN_VIDEO_RAM_SIZE  0x0400 // tile numbers,        0x4000-0x43FF
#define MSPACMAN_COLOR_RAM_SIZE  0x0400 // per-tile color index, 0x4400-0x47FF
#define MSPACMAN_WORK_RAM_SIZE   0x03F0 // general RAM,          0x4C00-0x4FEF
#define MSPACMAN_SPRITE_NUM_SIZE 0x0010 // sprite code+flip+color (spriteram),  0x4FF0-0x4FFF
#define MSPACMAN_SPRITE_POS_SIZE 0x0010 // sprite x/y (spriteram2),             0x5060-0x506F
#define MSPACMAN_SOUND_REG_SIZE  0x0020 // Namco WSG voice registers,          0x5040-0x505F

typedef struct {
    z80 cpu; // ArcadeCPU_Z80 -- registers/flags/cycle count; read_byte/
             // write_byte/port_in/port_out wired by mspacman_ports_wire().

    // rom[bank][addr] is directly addressable for any addr < 0xC000, so the
    // decode in mspacman_assets.cpp reads exactly like MAME's. Addresses
    // 0x4000-0x7FFF within a bank are never read (RAM/IO decodes first) and
    // are left zeroed.
    uint8_t rom[MSPACMAN_ROM_BANKS][MSPACMAN_ROM_BANK_SIZE];

    // Which bank the aux board's decoder currently has selected. Reset
    // value is MSPACMAN_BANK_DECRYPTED, matching init_mspacman()'s closing
    // `membank("bank1")->set_entry(1)` -- the board powers up showing Ms.
    // Pac-Man code, and the ROM switches to the plain Pac-Man bank only when
    // it touches one of the disable ranges. Booting with this at 0 gives a
    // machine that runs plain Pac-Man until something happens to touch
    // 0x3FF8-0x3FFF, which looks like a subtly wrong game rather than a
    // crash -- worth knowing before debugging that symptom.
    uint8_t bank;

    uint8_t video_ram[MSPACMAN_VIDEO_RAM_SIZE];
    uint8_t color_ram[MSPACMAN_COLOR_RAM_SIZE];
    uint8_t work_ram[MSPACMAN_WORK_RAM_SIZE];
    uint8_t sprite_num[MSPACMAN_SPRITE_NUM_SIZE];
    uint8_t sprite_pos[MSPACMAN_SPRITE_POS_SIZE];
    uint8_t sound_regs[MSPACMAN_SOUND_REG_SIZE];

    // 74LS259 "mainlatch" outputs (0x5000-0x5007 writes, one bit per
    // address per the ls259_device::write_d0 convention). The base
    // pacman() machine_config wires only q_out_cb<0/1/3/7> and comments
    // bits 2/4/5/6 as "hardware does not exist on any Pacman or Puckman
    // board I have seen" -- but mspacman(machine_config&) calls pacman()
    // and then adds ONE line of its own:
    //     m_mainlatch->q_out_cb<6>().set(FUNC(pacman_state::coin_lockout_global_w));
    // So bit 6 IS wired on this board, unlike plain Pac-Man. It gates the
    // coin slots on real hardware; there are no physical coin mechs here
    // (the sketch maps a button to COIN), so it is latched for fidelity and
    // deliberately not acted on -- see mspacman_ports.cpp.
    bool interrupt_enable; // bit 0
    bool sound_enable;     // bit 1
    bool flip_screen;      // bit 3
    bool coin_lockout;     // bit 6 -- Ms. Pac-Man only, see above
    bool coin_counter;     // bit 7

    // The Z80 runs in interrupt mode 0: real Pac-Man ROM code executes a
    // single `OUT (0),A` at startup to hand the CPU the exact opcode byte
    // (a single-byte RST instruction) to execute when the vblank interrupt
    // is acknowledged -- verified via MAME's pacman_interrupt_vector_w()/
    // interrupt_vector_r() and the Z80's set_irq_acknowledge_callback()
    // wiring. See mspacman_ports.cpp's port_out handler and
    // mspacman_run_frame()'s z80_gen_int() call.
    uint8_t interrupt_vector;

    // IN0/IN1/DSW1 shadow bytes, updated once per frame by
    // mspacman_input_update() (IN0/IN1) and mspacman_load_assets() (DSW1,
    // fixed at load time -- see mspacman_input.h). Bit layout verified
    // against MAME's INPUT_PORTS_START(mspacman), which is its own port
    // definition rather than a PORT_INCLUDE of pacman's -- the IN0/IN1 bits
    // match, but DSW1's defaults and its bit 7 do not (see
    // mspacman_assets.cpp). DSW2 doesn't exist on this
    // hardware (MAME: `PORT_BIT(0xff, IP_ACTIVE_HIGH, IPT_UNUSED)`) --
    // mspacman_ports.cpp returns a fixed 0xFF for it, no state needed here.
    uint8_t in0, in1, dsw1;

    uint8_t rotation; // 0=landscape 1=90 CCW 2=180 3=90 CW (portrait, the
                      // default -- see mspacman_init() for why it is 3 and
                      // not 1, which is what Invaders/LunarRescue use)
    bool    mirror_x; // horizontal mirror toggle (Pepper's Ghost cabinets)
} mspacman_system;

// Boot-error screen colors (RGB565) -- same convention as
// ArcadeMachine_Invaders's INVADERS_COLOR_ERROR_* constants.
#define MSPACMAN_COLOR_ERROR_NO_CARD   0xF800u // red    -- storage missing or won't mount
#define MSPACMAN_COLOR_ERROR_NO_ASSETS 0xFFE0u // yellow -- mounted, but ROM/PROM files missing

// Sets game-state defaults, wires the Z80 core's callbacks (see
// mspacman_ports.h), and initializes video (hal_video_init()). Does not
// touch storage.
void mspacman_init(mspacman_system *system);

// Loads ROM/PROM assets via ArcadeHAL's storage contract (see
// mspacman_assets.h), builds the tile/sprite/palette decode caches (see
// mspacman_video.h), and brings up audio. On success returns true; on
// failure returns false and sets *out_error_color to one of the
// MSPACMAN_COLOR_ERROR_* constants above.
bool mspacman_load_assets(mspacman_system *system, uint16_t *out_error_color);

// Runs exactly one video frame: executes Z80 cycles for one frame's worth
// of work (~50,688 cycles -- see mspacman_machine.cpp), fires the single
// per-frame vblank interrupt if `interrupt_enable` is set, then renders
// the frame via ArcadeHAL's video contract. Call this in a tight loop from
// the sketch after mspacman_input_update() has updated `system` for the
// frame.
void mspacman_run_frame(mspacman_system *system);

#ifdef __cplusplus
}
#endif

#endif
