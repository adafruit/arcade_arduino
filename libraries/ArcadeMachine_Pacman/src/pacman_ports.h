// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Pac-Man Z80 bus wiring: memory map (read_byte/write_byte) and the one
// real I/O-space access (port_out on port 0, for the interrupt vector).
//
// Unlike ArcadeCPU_i8080 (extern-global read_port()/write_port() the CPU
// core calls directly), ArcadeCPU_Z80's core wires hardware access via
// per-instance function pointers on the z80 struct itself -- so this file
// exposes a "wire" function that assigns them, rather than a fixed-name
// contract the CPU core reaches for by symbol name.
#ifndef PACMAN_PORTS_H
#define PACMAN_PORTS_H

#include "pacman_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

// Assigns system->cpu.{read_byte,write_byte,port_in,port_out,userdata}.
// Call once (from pacman_init()) before running the CPU.
void pacman_ports_wire(pacman_system *system);

#ifdef __cplusplus
}
#endif

#endif
