// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// CPU-facing I/O contract for the i8080 core's IN/OUT opcodes.
//
// This is deliberately NOT machine-aware -- no arcade_system, no game
// state. Any Machine library using ArcadeCPU_i8080 must provide C
// implementations of read_port()/write_port() with exactly this signature
// (see e.g. ArcadeMachine_Invaders's invaders_ports.h/.cpp). That's the
// whole "CPU axis" contract: a naming/signature convention, not a shared
// base type -- different CPUs (e.g. a future Z80 core) have genuinely
// different register shapes, so there's nothing else to share.
#ifndef I8080_PORTS_H
#define I8080_PORTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint8_t read_port(uint8_t port_number);
void write_port(uint8_t port_number, uint8_t port_data);

#ifdef __cplusplus
}
#endif

#endif
