// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Burger Time input mapping -- board-agnostic; the caller passes already
// debounced button states (the board backend's job, per arcade_hal_input.h).
#ifndef BTIME_INPUT_H
#define BTIME_INPUT_H

#include "btime_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

// Refreshes P1/P2/SYSTEM from this frame's button states, and handles the
// two meta controls (rotation cycle, mirror toggle) with edge detection.
// Also latches the coin IRQ on a coin press edge -- on this board that is
// the main CPU's ONLY interrupt source.
void btime_input_update(btime_system *system,
                        bool coin, bool start1, bool start2,
                        bool up, bool down, bool left, bool right,
                        bool pepper,
                        bool rotate_button, bool mirror_button);

#ifdef __cplusplus
}
#endif

#endif
