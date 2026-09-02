// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Space Invaders input mapping -- ported from invaders_pico's
// pico_input.c's handleInput(), minus the GPIO reads (see invaders_input.h).
#include "invaders_input.h"

void invaders_input_update(arcade_system *system,
                            bool coin, bool start1, bool start2,
                            bool left, bool right, bool shoot,
                            bool rotate_button, bool mirror_button) {
    // Cycle rotation on each press (edge detect, not hold).
    static bool rotate_prev = false;
    if (rotate_button && !rotate_prev)
        system->rotation = (system->rotation + 1u) & 3u;
    rotate_prev = rotate_button;

    // Toggle mirror_x on each press (edge detect, not hold).
    static bool mirror_prev = false;
    if (mirror_button && !mirror_prev)
        system->mirror_x = !system->mirror_x;
    mirror_prev = mirror_button;

    system->coin   = coin;
    system->start1 = start1;
    system->start2 = start2;
    system->left   = left;
    system->right  = right;
    system->shot   = shoot;
}
