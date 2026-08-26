// Lunar Rescue ROM/PROM loading. See lrescue_assets.h for why this can't
// reuse ArcadeMachine_Invaders' "sort filenames, load consecutively from
// 0x0000" convention -- addresses below are taken directly from MAME's
// ROM_START( lrescue ) in midw8080/8080bw.cpp.
#include <stdio.h>
#include <string.h>
#include "lrescue_assets.h"
#include "arcade_hal_storage.h"

typedef struct {
    const char *filename;
    uint32_t    address;
    uint32_t    size;
} rom_chip_t;

// lrescue.1-4 are the standard consecutive 8K bank Space Invaders also uses;
// lrescue.5-6 sit at 0x4000, past the 0x2000-0x3fff RAM window -- real ROM
// on this board, NOT the address-bus alias Space Invaders' PCB has at that
// range (see lrescue_machine.cpp's comment on mirror_2000_at_4000).
static const rom_chip_t ROM_CHIPS[] = {
    { "lrescue.1", 0x0000, 0x0800 },
    { "lrescue.2", 0x0800, 0x0800 },
    { "lrescue.3", 0x1000, 0x0800 },
    { "lrescue.4", 0x1800, 0x0800 },
    { "lrescue.5", 0x4000, 0x0800 },
    { "lrescue.6", 0x4800, 0x0800 },
};
#define NUM_ROM_CHIPS (sizeof(ROM_CHIPS) / sizeof(ROM_CHIPS[0]))

// 7643-1.cpu -- NOT program code (confirmed: MAME loads it into a "proms"
// region tagged "color map", not the CPU's address map at all). It lives in
// /prom/, not /rom/, and must never be swept into CPU memory alongside the
// six chips above.
#define COLOR_PROM_PATH "/prom/7643-1.cpu"

static int load_rom_chips(uint8_t *memory) {
    int loaded = 0;
    for (unsigned i = 0; i < NUM_ROM_CHIPS; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/rom/%s", ROM_CHIPS[i].filename);

        hal_file_t *f = hal_storage_open(path);
        if (!f) continue;

        uint32_t br = hal_storage_read(f, memory + ROM_CHIPS[i].address, ROM_CHIPS[i].size);
        hal_storage_close(f);
        if (br > 0) loaded++;
    }
    return loaded;
}

// Missing/short PROM data just leaves color_prom zeroed -- lrescue_video.cpp
// then reads palette index 0 for every block (a real but silent visual
// degradation), not a boot-error condition. The color PROM is cosmetic;
// the program ROM chips above are not.
static void load_color_prom(uint8_t *color_prom) {
    memset(color_prom, 0, LRESCUE_COLOR_PROM_SIZE);
    hal_file_t *f = hal_storage_open(COLOR_PROM_PATH);
    if (!f) return;
    hal_storage_read(f, color_prom, LRESCUE_COLOR_PROM_SIZE);
    hal_storage_close(f);
}

lrescue_rom_load_status_t lrescue_load_rom(arcade_system *system) {
    if (!hal_storage_mount()) {
        return LRESCUE_ROM_LOAD_NO_STORAGE;
    }

    int loaded = load_rom_chips(system->state.memory);
    // Require at least the four low chips (0x0000-0x1fff, which contain the
    // reset vector) -- a set missing those can't run at all. lrescue.5/.6
    // missing would still boot but misbehave later; we don't special-case
    // that here, matching ArcadeMachine_Invaders' "best effort" philosophy.
    if (loaded < 4) {
        hal_storage_unmount();
        return LRESCUE_ROM_LOAD_NO_ROM_FILES;
    }

    load_color_prom(system->color_prom);
    // Leave storage mounted -- lrescue_audio_load_samples() loads WAV files
    // next, then the caller unmounts.

    // DIP-equivalent defaults. Lives: sicv_base's IN2 bits 0-1 encode
    // 00=3, 01=4, 02=5, 03=6 lives -- default to 3, same as Invaders' own
    // default. Bits 3 and 7 are DIPUNUSED/factory-fixed in MAME's
    // INPUT_PORTS_START(lrescue) (bonus life fixed at 1500 -> bit3=0, "coin
    // info" fixed on -> bit7=0); read_port() computes those bits as
    // (1 - dip_switches[n]), so dip_switches[3]=dip_switches[7]=1 here
    // produces the required fixed 0.
    system->dip_switches[0] = 1;
    system->dip_switches[1] = 1;
    system->dip_switches[2] = 0; // unused
    system->dip_switches[3] = 1; // bonus life fixed at 1500 (factory default)
    system->dip_switches[4] = 0; // unused (P2 controls, cocktail-only)
    system->dip_switches[5] = 0;
    system->dip_switches[6] = 0;
    system->dip_switches[7] = 1; // "coin info" fixed on (factory default)

    return LRESCUE_ROM_LOAD_OK;
}
