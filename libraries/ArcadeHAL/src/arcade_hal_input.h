// ArcadeHAL: input contract.
//
// A board backend exposes its physical buttons as a flat array of raw,
// already-debounced-or-not (board's choice) boolean levels, indexed by a
// board-defined constant (e.g. board_config_fruitjam.h's HAL_BTN_* enum).
// Which raw index means which *game* action (coin, shoot, joystick left...)
// is NOT decided here -- that mapping is inherently a one-off decision for
// a specific Machine+Board pairing, so it lives in the sketch (the SAMP
// composition root), not in this contract or in either library.
#ifndef ARCADE_HAL_INPUT_H
#define ARCADE_HAL_INPUT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Number of raw buttons this board exposes. Board-defined.
extern const uint8_t HAL_INPUT_BUTTON_COUNT;

void hal_input_init(void);

// Reads raw button `index` (0..HAL_INPUT_BUTTON_COUNT-1). true = pressed.
// Active-low/pull-up handling, if any, is the board backend's concern.
bool hal_input_read(uint8_t index);

#ifdef __cplusplus
}
#endif

#endif
