// Host-harness pico/stdlib.h shim -- see ../../README.md.
//
// ArcadeCPU_i8080's i8080.c includes this for exactly one symbol:
// tight_loop_contents(), used by its cpu_panic() spin (a port artifact --
// the upstream shotto42/invaders code called exit() there, and the Pico port
// replaced that with a halt because there is no OS to exit to). That panic
// is currently unreachable: undocumented opcodes and HLT are handled as
// 4-cycle NOPs instead, precisely so a bad byte cannot wedge Core 0 (see
// invaders_pico's DEVNOTES.md problem #1).
//
// ArcadeCPU_Z80 needs no equivalent, which is why this shim did not exist
// until the Invaders harness was built.
#ifndef PICO_STDLIB_H_HOST_SHIM
#define PICO_STDLIB_H_HOST_SHIM

#ifndef tight_loop_contents
#define tight_loop_contents() ((void)0)
#endif

#endif
