// Space Invaders machine lifecycle -- ported from invaders_pico's arcade.c.
// Orchestrates ArcadeCPU_i8080 + this machine's own port/video/audio/asset
// modules, talking to hardware only through ArcadeHAL.
#include <string.h>
#include "invaders_machine.h"
#include "invaders_ports.h"
#include "invaders_video.h"
#include "invaders_audio.h"
#include "invaders_assets.h"
#include "arcade_hal_video.h"
#include "arcade_hal_audio.h"
#include "arcade_hal_storage.h"
#include "arcade_hal_input.h"

#define FRAMERATE          59.541985
#define CYCLES_PER_FRAME   (1996800.0 / FRAMERATE) // ~33,536 cycles/frame

void invaders_init(arcade_system *system) {
    memset(&system->state, 0, sizeof(system->state));

    system->left   = 0;
    system->right  = 0;
    system->shot   = 0;
    system->start1 = 0;
    system->start2 = 0;
    system->coin   = 0;
    system->tilt   = 0;

    system->ext_shift_offset = 0;
    system->ext_shift_data   = 0;
    system->cocktail_vertical_screen_flip = 0;
    system->rotation = 1;   // default: 90 deg CCW tate mode
    system->mirror_x = false;

    hal_video_init();
    invaders_ports_bind(system);
}

bool invaders_load_assets(arcade_system *system, uint16_t *out_error_color) {
    invaders_rom_load_status_t rom_status = invaders_load_rom(system);
    if (rom_status == INVADERS_ROM_LOAD_NO_STORAGE) {
        *out_error_color = INVADERS_COLOR_ERROR_NO_CARD;
        return false;
    }
    if (rom_status == INVADERS_ROM_LOAD_NO_ROM_FILES) {
        *out_error_color = INVADERS_COLOR_ERROR_NO_ASSETS;
        return false;
    }

    hal_audio_init(INVADERS_AUDIO_SAMPLE_RATE);
    int samples_loaded = invaders_audio_load_samples();
    hal_storage_unmount();
    if (samples_loaded == 0) {
        *out_error_color = INVADERS_COLOR_ERROR_NO_ASSETS;
        return false;
    }

    hal_input_init();
    return true;
}

void invaders_run_frame(arcade_system *system) {
    // `cyc` persists across frames -- any cycles run past this frame's
    // budget are carried forward and subtracted from the next frame's
    // budget, matching the reference clone's timing exactly.
    static int cyc = 0;
    int int_state = 0;
    while (int_state != 2) {
        cyc += exec_opcode(&system->state);
        if (cyc >= (int)(CYCLES_PER_FRAME / 2) && int_state == 0) {
            int_state = 1;
            cyc += interrupt(&system->state, 1);
        }
        if (cyc >= (int)CYCLES_PER_FRAME && int_state == 1) {
            int_state = 2;
            cyc += interrupt(&system->state, 2);
        }
    }
    cyc = (int)CYCLES_PER_FRAME - cyc;

    invaders_draw_frame(system);
}
