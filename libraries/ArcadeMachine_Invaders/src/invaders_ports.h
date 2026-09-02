// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// i8080 port I/O for the Space Invaders machine.
// Ported from invaders_pico's i8080_ports.c (itself identical to
// shotto42/invaders except for the sound backend call).
#ifndef INVADERS_PORTS_H
#define INVADERS_PORTS_H

#include <stdint.h>
#include "invaders_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

// Binds the port I/O functions below to a specific system instance. Call
// once before running the CPU (ArcadeCPU_i8080 calls read_port()/
// write_port() with no other way to reach `system`).
void invaders_ports_bind(arcade_system *system);

// These match the signatures ArcadeCPU_i8080's exec_opcode() calls via
// IN/OUT opcodes (see i8080_ports.h in that library).
uint8_t read_port(uint8_t port_number);
void write_port(uint8_t port_number, uint8_t port_data);

#ifdef __cplusplus
}
#endif

#endif
