// Host-harness pico.h shim -- see ../README.md.
//
// Some ArcadeMachine_* audio files include <pico.h> for one thing only:
// __not_in_flash_func(), which places an ISR-called function in SRAM so it
// never stalls on an XIP flash fetch. That is a deliberate, documented
// exception to SAMP's board-agnostic rule (see ArcadeMachine_Invaders's
// invaders_audio.cpp and DEVNOTES.md problem #7).
//
// On the host there is no flash/XIP distinction, so these degrade to
// plain passthroughs and the audio sources compile unmodified -- which is
// preferable to excluding them from the harness build, since it keeps them
// under compiler coverage.
#ifndef PICO_H_HOST_SHIM
#define PICO_H_HOST_SHIM

#ifndef __not_in_flash_func
#define __not_in_flash_func(func_name) func_name
#endif
#ifndef __time_critical_func
#define __time_critical_func(func_name) func_name
#endif
#ifndef __not_in_flash
#define __not_in_flash(group)
#endif
#ifndef __not_in_flash_data
#define __not_in_flash_data(group)
#endif

#endif
