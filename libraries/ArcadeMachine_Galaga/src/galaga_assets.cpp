// Galaga ROM/PROM loading -- see galaga_assets.h for the citation trail.
#include <stdio.h>
#include <string.h>
#include "galaga_assets.h"
#include "galaga_video.h"
#include "galaga_audio.h"
#include "arcade_hal_storage.h"
#include <Arduino.h> // DEBUG: Serial tracing for the red-screen investigation below --
                      // pull this (and the prints it guards) once the cause is found
                      // and noted in DEVNOTES.md.

typedef struct {
    const char *filename;
    uint8_t    *dest;
    uint32_t    size;
} rom_file_t;

static bool load_manifest(const rom_file_t *files, unsigned count) {
    int loaded = 0;
    for (unsigned i = 0; i < count; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/rom/%s", files[i].filename);

        hal_file_t *f = hal_storage_open(path);
        if (!f) {
            Serial.print("[galaga]   ");
            Serial.print(path);
            Serial.println(": open FAILED");
            continue;
        }

        uint32_t br = hal_storage_read(f, files[i].dest, files[i].size);
        hal_storage_close(f);
        bool ok = (br == files[i].size);
        if (ok) loaded++;
        Serial.print("[galaga]   ");
        Serial.print(path);
        Serial.print(": read ");
        Serial.print(br);
        Serial.print("/");
        Serial.print(files[i].size);
        Serial.println(ok ? " OK" : " SHORT/MISMATCH");
    }
    return loaded == (int)count;
}

galaga_rom_load_status_t galaga_load_rom(galaga_system *system) {
    Serial.println("[galaga] galaga_load_rom(): calling hal_storage_mount()...");
    bool mounted = hal_storage_mount();
    Serial.print("[galaga] hal_storage_mount() returned ");
    Serial.println(mounted ? "true" : "false");
    if (!mounted) {
        return GALAGA_ROM_LOAD_NO_STORAGE;
    }

    // Program ROM -- all 3 CPUs. SHA1-verified this session to be an
    // exact match for MAME's ROM_START(galaga) ("Galaga, Namco rev. B"),
    // not just filename-matched.
    const rom_file_t program_rom[] = {
        { "gg1-1b.3p", system->rom_main + 0x0000, 0x1000 },
        { "gg1-2b.3m", system->rom_main + 0x1000, 0x1000 },
        { "gg1-3.2m",  system->rom_main + 0x2000, 0x1000 },
        { "gg1-4b.2l", system->rom_main + 0x3000, 0x1000 },
        { "gg1-5b.3f", system->rom_sub,           0x1000 },
        { "gg1-7b.2c", system->rom_sub2,          0x1000 },
    };
    Serial.println("[galaga] loading program ROM (6 files)...");
    if (!load_manifest(program_rom, 6)) {
        Serial.println("[galaga] program ROM load FAILED -- unmounting, GALAGA_ROM_LOAD_NO_ROM_FILES");
        hal_storage_unmount();
        return GALAGA_ROM_LOAD_NO_ROM_FILES;
    }

    // Graphics ROMs -- gg1-9.4l (tiles, gfx1) then gg1-11.4d + gg1-10.4f
    // (sprites, gfx2, concatenated in that order per MAME's ROM_LOAD
    // offsets 0x0000/0x1000 within the gfx2 region). Non-fatal if
    // missing/short, same precedent as every other port's gfx assets.
    memset(galaga_gfx1_rom, 0, GALAGA_GFX1_SIZE);
    memset(galaga_gfx2_rom, 0, GALAGA_GFX2_SIZE);
    const rom_file_t gfx_rom[] = {
        { "gg1-9.4l",  galaga_gfx1_rom + 0x0000, 0x1000 },
        { "gg1-11.4d", galaga_gfx2_rom + 0x0000, 0x1000 },
        { "gg1-10.4f", galaga_gfx2_rom + 0x1000, 0x1000 },
    };
    Serial.println("[galaga] loading gfx ROM (3 files, non-fatal if missing)...");
    load_manifest(gfx_rom, 3);

    // Color PROMs -- prom-5.5n (32B palette), prom-4.2n (256B char
    // lookup), prom-3.1c (256B sprite lookup). The "namco" sound-PROM
    // region is loaded separately just below.
    memset(galaga_palette_prom, 0, GALAGA_PALETTE_PROM_SIZE);
    memset(galaga_char_lookup_prom, 0, GALAGA_CHAR_LOOKUP_SIZE);
    memset(galaga_sprite_lookup_prom, 0, GALAGA_SPRITE_LOOKUP_SIZE);
    const rom_file_t color_proms[] = {
        { "prom-5.5n", galaga_palette_prom,      GALAGA_PALETTE_PROM_SIZE },
        { "prom-4.2n", galaga_char_lookup_prom,   GALAGA_CHAR_LOOKUP_SIZE },
        { "prom-3.1c", galaga_sprite_lookup_prom, GALAGA_SPRITE_LOOKUP_SIZE },
    };
    Serial.println("[galaga] loading color PROMs (3 files, non-fatal if missing)...");
    load_manifest(color_proms, 3);

    // Namco WSG waveform PROM. MAME's ROM_START(galaga) puts this at offset
    // 0 of the "namco" region; the other 256 bytes there (prom-2.5c) are
    // labelled "timing - not used" in MAME itself, so nothing loads them.
    // Non-fatal like the other PROMs: a missing wave PROM means silence,
    // not a dead machine.
    const rom_file_t wave_prom[] = {
        { "prom-1.1d", galaga_wave_prom, GALAGA_WAVE_PROM_SIZE },
    };
    Serial.println("[galaga] loading WSG waveform PROM (1 file, non-fatal if missing)...");
    load_manifest(wave_prom, 1);

    // DSWA/DSWB defaults -- verified against MAME's INPUT_PORTS_START(galaga):
    // DSWA = Difficulty:Easy(0x03) | UNUSED(0x04) | Demo_Sounds:On(0x00)
    //      | Freeze:Off(0x10) | Rack_Test:Off(0x20) | UNUSED(0x40)
    //      | Cabinet:Upright(0x80) = 0xF7
    // DSWB = Coinage:1C_1C(0x07) | Bonus_Life:"20K,70K,Every 70K"(0x10)
    //      | Lives:3(0x80) = 0x97
    // Read via bosco_dsw_r() at 0x6800-0x6807 (see galaga_ports.cpp) --
    // NOT a flat byte read. Getting this wrong (leaving it zeroed) forces
    // Rack Test (self-test/diagnostic) mode on, since that DIP defaults
    // to its "off" state being bit=1, not bit=0 -- exactly the real bug
    // found bringing this up on hardware (see galaga_machine.h's comment
    // on the dswa/dswb fields).
    //
    // The two "unused" DSWA bits (0x04 and 0x40) are NOT free to leave at
    // 0, which an earlier version of this file did (giving 0xB3). MAME
    // declares them `PORT_DIPUNUSED_DIPLOC(mask, IP_ACTIVE_LOW, ...)`, and
    // that macro's second argument is the switch's DEFAULT: with
    // IP_ACTIVE_LOW the default is the mask itself, i.e. both read back as
    // 1, not 0. "Unused" describes what the SWITCH does, not what the CPU
    // reads. Bit 2 in particular is load-bearing: sub CPU's task-0x0A
    // handler (sub ROM 0x0ECA) reads it via 0x6802 and takes `RET nz` --
    // the normal path -- only when it is SET. With it clear, that handler
    // instead falls into a routine that reads UNMAPPED address space
    // (0x10FF/0x10DF, above sub's 4K ROM), XORs two reads of the same
    // address expecting them to differ in bit 4, and executes `RST 0` --
    // a jump to 0x0000, i.e. a software RESET of the sub CPU -- when they
    // don't. Since an emulated unmapped read returns a stable constant,
    // that reset fired every single frame, restarting sub's boot forever
    // and deadlocking the 3-CPU handshake main waits on at 0x35F3. Found
    // via the host harness (arcade_arduino/tools/galaga_host), not on
    // hardware.
    system->dswa = 0xF7;
    system->dswb = 0x97;

    // Leave storage mounted -- galaga_load_assets() unmounts once
    // galaga_video_build_caches() has consumed the staging buffers above.
    Serial.println("[galaga] galaga_load_rom(): GALAGA_ROM_LOAD_OK");
    return GALAGA_ROM_LOAD_OK;
}
