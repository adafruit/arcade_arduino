// Adafruit Fruit Jam (RP2350B) board facts: pins, addresses, raw button
// indices. No game logic lives here -- see invaders_pico's README.md
// "Controls" table for the physical pinout this mirrors.
#ifndef BOARD_CONFIG_FRUITJAM_H
#define BOARD_CONFIG_FRUITJAM_H

#ifdef __cplusplus
extern "C" {
#endif

// Raw button indices exposed via hal_input_read(). A sketch maps these to
// game-semantic actions (see invaders_fruitjam.ino) -- this board backend
// only knows "here are N buttons, here's how to read each one".
enum {
    HAL_BTN_ROTATE = 0, // GPIO 4  (Button 2)  -- cycle screen rotation
    HAL_BTN_MIRROR,     // GPIO 5  (Button 3)  -- toggle horizontal mirror
    HAL_BTN_COIN,       // GPIO 45 (header A5)
    HAL_BTN_START1,     // GPIO 6  (header D6)
    HAL_BTN_START2,     // GPIO 7  (header D7)
    HAL_BTN_LEFT,       // GPIO 8  (header D8)
    HAL_BTN_RIGHT,      // GPIO 9  (header D9)
    HAL_BTN_SHOOT,      // GPIO 10 (header D10)
};

// I2C0 (GPIO 20/21) -- TLV320DAC3100 DAC control
#define FRUITJAM_I2C_SDA_PIN     20
#define FRUITJAM_I2C_SCL_PIN     21
#define FRUITJAM_CODEC_RESET_PIN 22 // shared with the onboard ESP32-C6; hold high
#define FRUITJAM_DAC_I2C_ADDR    0x18

// PIO1 I2S output to the DAC
#define FRUITJAM_I2S_DIN_PIN     24
#define FRUITJAM_I2S_BCLK_PIN    26 // WS = BCLK + 1 = 27

// SPI0 -- microSD card (wili8jam SD driver, see sdcard.c)
#define FRUITJAM_SD_SCK_PIN      34
#define FRUITJAM_SD_MOSI_PIN     35
#define FRUITJAM_SD_MISO_PIN     36
#define FRUITJAM_SD_CS_PIN       39
#define FRUITJAM_SD_CD_PIN       33

#ifdef __cplusplus
}
#endif

#endif
