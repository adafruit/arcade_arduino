// ArcadeMachine_Pacman: top-level Pac-Man machine state + lifecycle.
//
// The project's first Z80-based machine -- built from scratch against the
// real arcade ROM/PROM dump (pacman_assets/rom/), not ported from any
// existing emulator. Every hardware fact used across this library's files
// (memory map, I/O map, interrupt scheme, tile/sprite decode, palette
// decode, WSG sound registers) was verified directly against MAME's own
// `pacman` driver source (src/mame/pacman/pacman.cpp, pacman_v.cpp,
// devices/sound/namco.cpp/.h, upstream project mamedev/mame) rather than
// inferred from memory or copied from picopacman-main (investigated and
// found to contain no CPU/hardware emulation at all -- see the top-level
// project notes). Each file below cites the specific MAME construct its
// facts came from, same rigor ArcadeMachine_LunarRescue's README applies
// to its own ROM/PROM map.
//
// Board-agnostic: talks only to ArcadeHAL and ArcadeCPU_Z80, never to a
// specific board's libraries.
#ifndef PACMAN_MACHINE_H
#define PACMAN_MACHINE_H

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
#define PACMAN_GAME_WIDTH  288
#define PACMAN_GAME_HEIGHT 224

// Memory region sizes, drawn directly from MAME's pacman_map() address map
// (src/mame/pacman/pacman.cpp) -- see pacman_ports.cpp's header comment for
// the exact address ranges each one backs.
#define PACMAN_ROM_SIZE        0x4000 // 4x 4K program ROM chips, 0x0000-0x3FFF
#define PACMAN_VIDEO_RAM_SIZE  0x0400 // tile numbers,        0x4000-0x43FF
#define PACMAN_COLOR_RAM_SIZE  0x0400 // per-tile color index, 0x4400-0x47FF
#define PACMAN_WORK_RAM_SIZE   0x03F0 // general RAM,          0x4C00-0x4FEF
#define PACMAN_SPRITE_NUM_SIZE 0x0010 // sprite code+flip+color (spriteram),  0x4FF0-0x4FFF
#define PACMAN_SPRITE_POS_SIZE 0x0010 // sprite x/y (spriteram2),             0x5060-0x506F
#define PACMAN_SOUND_REG_SIZE  0x0020 // Namco WSG voice registers,          0x5040-0x505F

typedef struct {
    z80 cpu; // ArcadeCPU_Z80 -- registers/flags/cycle count; read_byte/
             // write_byte/port_in/port_out wired by pacman_ports_wire().

    uint8_t rom[PACMAN_ROM_SIZE];
    uint8_t video_ram[PACMAN_VIDEO_RAM_SIZE];
    uint8_t color_ram[PACMAN_COLOR_RAM_SIZE];
    uint8_t work_ram[PACMAN_WORK_RAM_SIZE];
    uint8_t sprite_num[PACMAN_SPRITE_NUM_SIZE];
    uint8_t sprite_pos[PACMAN_SPRITE_POS_SIZE];
    uint8_t sound_regs[PACMAN_SOUND_REG_SIZE];

    // 74LS259 "mainlatch" outputs (0x5000-0x5007 writes, one bit per
    // address per the ls259_device::write_d0 convention) -- only the 4
    // bits real Pac-Man/Puckman PCBs actually wire up. Verified against
    // MAME's pacman_state::pacman() machine_config, which wires only
    // q_out_cb<0/1/3/7> and explicitly comments bits 2/4/5/6 as
    // "hardware does not exist on any Pacman or Puckman board I have
    // seen" (lamps/coin-lockout are a different, unrelated board variant).
    bool interrupt_enable; // bit 0
    bool sound_enable;     // bit 1
    bool flip_screen;      // bit 3
    bool coin_counter;     // bit 7

    // The Z80 runs in interrupt mode 0: real Pac-Man ROM code executes a
    // single `OUT (0),A` at startup to hand the CPU the exact opcode byte
    // (a single-byte RST instruction) to execute when the vblank interrupt
    // is acknowledged -- verified via MAME's pacman_interrupt_vector_w()/
    // interrupt_vector_r() and the Z80's set_irq_acknowledge_callback()
    // wiring. See pacman_ports.cpp's port_out handler and
    // pacman_run_frame()'s z80_gen_int() call.
    uint8_t interrupt_vector;

    // IN0/IN1/DSW1 shadow bytes, updated once per frame by
    // pacman_input_update() (IN0/IN1) and pacman_load_assets() (DSW1,
    // fixed at load time -- see pacman_input.h). Bit layout verified
    // against MAME's INPUT_PORTS_START(pacman). DSW2 doesn't exist on this
    // hardware (MAME: `PORT_BIT(0xff, IP_ACTIVE_HIGH, IPT_UNUSED)`) --
    // pacman_ports.cpp returns a fixed 0xFF for it, no state needed here.
    uint8_t in0, in1, dsw1;

    uint8_t rotation; // 0=landscape 1=90 CCW (tate, default) 2=180 3=90 CW
    bool    mirror_x; // horizontal mirror toggle (Pepper's Ghost cabinets)
} pacman_system;

// Boot-error screen colors (RGB565) -- same convention as
// ArcadeMachine_Invaders's INVADERS_COLOR_ERROR_* constants.
#define PACMAN_COLOR_ERROR_NO_CARD   0xF800u // red    -- storage missing or won't mount
#define PACMAN_COLOR_ERROR_NO_ASSETS 0xFFE0u // yellow -- mounted, but ROM/PROM files missing

// Sets game-state defaults, wires the Z80 core's callbacks (see
// pacman_ports.h), and initializes video (hal_video_init()). Does not
// touch storage.
void pacman_init(pacman_system *system);

// Loads ROM/PROM assets via ArcadeHAL's storage contract (see
// pacman_assets.h), builds the tile/sprite/palette decode caches (see
// pacman_video.h), and brings up audio. On success returns true; on
// failure returns false and sets *out_error_color to one of the
// PACMAN_COLOR_ERROR_* constants above.
bool pacman_load_assets(pacman_system *system, uint16_t *out_error_color);

// Runs exactly one video frame: executes Z80 cycles for one frame's worth
// of work (~50,688 cycles -- see pacman_machine.cpp), fires the single
// per-frame vblank interrupt if `interrupt_enable` is set, then renders
// the frame via ArcadeHAL's video contract. Call this in a tight loop from
// the sketch after pacman_input_update() has updated `system` for the
// frame.
void pacman_run_frame(pacman_system *system);

#ifdef __cplusplus
}
#endif

#endif
