// Galaga input mapping implementation -- see galaga_input.h for the bit
// layout citation.
#include "galaga_input.h"

void galaga_input_update(galaga_system *system,
                          bool coin, bool start1, bool start2,
                          bool left, bool right, bool fire,
                          bool rotate_button, bool mirror_button) {
    system->in0 = (uint8_t)(0xFF
        & ~(right ? 0x02 : 0)
        & ~(left  ? 0x08 : 0));

    system->in1 = (uint8_t)(0xFF
        & ~(fire   ? 0x01 : 0)
        & ~(start1 ? 0x04 : 0)
        & ~(start2 ? 0x08 : 0)
        & ~(coin   ? 0x10 : 0));

    // Hand the buttons to the 51XX, which owns coin/credit bookkeeping and
    // assembles the per-player control bytes the game actually reads (see
    // galaga_51xx.h). in0/in1 above stay as the raw shadow for the direct
    // DIP/port reads; Galaga's controls reach the game only via the 51XX.
    galaga_51xx_set_inputs(&system->io51, coin, start1, start2, left, right, fire);

    // Edge-detected meta controls -- same shape as every other
    // ArcadeMachine_*'s rotate/mirror handling (e.g. pacman_input.cpp).
    static bool prev_rotate = false, prev_mirror = false;
    if (rotate_button && !prev_rotate) system->rotation = (system->rotation + 1) & 0x03;
    if (mirror_button && !prev_mirror) system->mirror_x = !system->mirror_x;
    prev_rotate = rotate_button;
    prev_mirror = mirror_button;
}
