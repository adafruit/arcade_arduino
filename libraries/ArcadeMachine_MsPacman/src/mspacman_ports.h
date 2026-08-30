// Ms. Pac-Man Z80 bus wiring: the BANKED memory map (read_byte/write_byte,
// including the aux board's address-triggered bank switching) and the one
// real I/O-space access (port_out on port 0, for the interrupt vector).
// See mspacman_ports.cpp's header for the full map and its MAME citations.
//
// Unlike ArcadeCPU_i8080 (extern-global read_port()/write_port() the CPU
// core calls directly), ArcadeCPU_Z80's core wires hardware access via
// per-instance function pointers on the z80 struct itself -- so this file
// exposes a "wire" function that assigns them, rather than a fixed-name
// contract the CPU core reaches for by symbol name.
#ifndef MSPACMAN_PORTS_H
#define MSPACMAN_PORTS_H

#include "mspacman_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

// Assigns system->cpu.{read_byte,write_byte,port_in,port_out,userdata}.
// Call once (from mspacman_init()) before running the CPU.
void mspacman_ports_wire(mspacman_system *system);

#ifdef __cplusplus
}
#endif

#endif
