// hal_input.h implementation for Adafruit Fruit Jam.
// Ported from invaders_pico's pico_input.c (minus game-semantic mapping,
// which now lives in the sketch -- see invaders_fruitjam.ino).
#include <stdint.h>
#include "hardware/gpio.h"
#include "arcade_hal_input.h"
#include "board_config_fruitjam.h"

// Indexed by the HAL_BTN_* enum from board_config_fruitjam.h, keeping the
// pin table and the enum from silently drifting apart.
static const uint8_t pins[] = {
    [HAL_BTN_ROTATE] = 4,
    [HAL_BTN_MIRROR] = 5,
    [HAL_BTN_COIN]   = 45,
    [HAL_BTN_START1] = 6,
    [HAL_BTN_START2] = 7,
    [HAL_BTN_LEFT]   = 8,
    [HAL_BTN_RIGHT]  = 9,
    [HAL_BTN_SHOOT]  = 10,
    [HAL_BTN_UP]     = 43,
    [HAL_BTN_DOWN]   = 44,
};

const uint8_t HAL_INPUT_BUTTON_COUNT = sizeof(pins) / sizeof(pins[0]);

void hal_input_init(void) {
    for (uint8_t i = 0; i < HAL_INPUT_BUTTON_COUNT; i++) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_IN);
        gpio_pull_up(pins[i]);
    }
}

bool hal_input_read(uint8_t index) {
    if (index >= HAL_INPUT_BUTTON_COUNT) return false;
    return !gpio_get(pins[index]); // active-low
}
