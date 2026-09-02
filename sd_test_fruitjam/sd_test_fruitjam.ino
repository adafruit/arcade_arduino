// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Standalone smoke test for ArcadeBoard_FruitJam's hal_storage implementation.
//
// Mounts the SD card, lists /rom/ and /samples/, and reads the first few
// bytes of /rom/invaders.h -- no CPU emulator, no video, no audio.
// Exercises the real production hal_storage_fruitjam.cpp (raw SPI SD
// driver + vendored FatFs) in isolation.
//
// Expected result, with a card formatted FAT32/MBR (not GPT/exFAT --
// see invaders_pico's README.md "ROM and audio samples" section for why
// that distinction matters) and invaders_pico's rom/+samples/ folders
// copied on: a directory listing of both folders, then 16 hex bytes read
// from /rom/invaders.h. "FAILED to mount" almost always means the card
// isn't MBR/FAT32; "(could not open /rom/)" means the folder is missing or
// misnamed.
#include <arcade_hal_storage.h>
// arduino-cli discovers libraries to link by scanning #include directives,
// not library.properties `depends=` -- this include is what actually pulls
// ArcadeBoard_FruitJam's hal_storage_fruitjam.cpp (the real implementation
// of the functions below) into the build.
#include <board_config_fruitjam.h>

static void print_entry(const char *filename, void *ctx) {
    (void)ctx;
    Serial.print("  ");
    Serial.println(filename);
}

void setup() {
    Serial.begin(115200);
    delay(2000); // give the Serial Monitor time to connect before we print

    Serial.println("Mounting SD card...");
    if (!hal_storage_mount()) {
        Serial.println("FAILED to mount SD card. Check it's inserted and");
        Serial.println("formatted FAT32 with an MBR partition scheme (not GPT/exFAT).");
        return;
    }
    Serial.println("Mounted OK.");

    Serial.println("/rom/ contents:");
    if (!hal_storage_list_dir("/rom", print_entry, NULL))
        Serial.println("  (could not open /rom/)");

    Serial.println("/samples/ contents:");
    if (!hal_storage_list_dir("/samples", print_entry, NULL))
        Serial.println("  (could not open /samples/)");

    hal_file_t *f = hal_storage_open("/rom/invaders.h");
    if (f) {
        uint8_t buf[16];
        uint32_t n = hal_storage_read(f, buf, sizeof(buf));
        hal_storage_close(f);
        Serial.print("Read ");
        Serial.print(n);
        Serial.println(" bytes from /rom/invaders.h:");
        for (uint32_t i = 0; i < n; i++) {
            if (buf[i] < 0x10) Serial.print('0');
            Serial.print(buf[i], HEX);
            Serial.print(' ');
        }
        Serial.println();
    } else {
        Serial.println("Could not open /rom/invaders.h (check filename/case on the card).");
    }

    hal_storage_unmount();
    Serial.println("Done.");
}

void loop() {}
