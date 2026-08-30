// Galaga 3x Z80 bus wiring -- see galaga_ports.cpp for the full memory map
// citation and the 06XX mux implementation.
#ifndef GALAGA_PORTS_H
#define GALAGA_PORTS_H

#include "galaga_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

// Wires all 3 z80 instances' read_byte/write_byte/port_in/port_out
// function pointers + userdata. Call once, from galaga_init().
void galaga_ports_wire(galaga_system *system);

#ifdef __cplusplus
}
#endif

#endif
