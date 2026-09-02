// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// i8080 port I/O for the Lunar Rescue machine.
// See lrescue_ports.cpp for the MAME source citations behind every bit.
#ifndef LRESCUE_PORTS_H
#define LRESCUE_PORTS_H

#include <stdint.h>
#include "lrescue_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

// Binds the port I/O functions below to a specific system instance. Call
// once before running the CPU (ArcadeCPU_i8080 calls read_port()/
// write_port() with no other way to reach `system`).
void lrescue_ports_bind(arcade_system *system);

// These match the signatures ArcadeCPU_i8080's exec_opcode() calls via
// IN/OUT opcodes. NOTE: these are the same global symbol names
// ArcadeMachine_Invaders defines -- exactly one Machine library may ever be
// linked into a given sketch (the SAMP composition root picks one), so this
// is not a collision, it's the intended pattern.
uint8_t read_port(uint8_t port_number);
void write_port(uint8_t port_number, uint8_t port_data);

#ifdef __cplusplus
}
#endif

#endif
