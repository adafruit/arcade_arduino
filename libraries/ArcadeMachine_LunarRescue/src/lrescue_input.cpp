// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Lunar Rescue input mapping -- structurally identical to
// ArcadeMachine_Invaders' invaders_input.cpp; see lrescue_input.h for why
// left/right + a single fire button is the correct (not simplified)
// control scheme for this game.
#include "lrescue_input.h"

void lrescue_input_update(arcade_system *system,
                           bool coin, bool start1, bool start2,
                           bool left, bool right, bool shot,
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
    system->shot   = shot;
}
