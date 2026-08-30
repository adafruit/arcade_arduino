// Donkey Kong Z80 bus wiring: memory map (read_byte/write_byte), the i8257
// DMA controller, and the video-control latch block. See dkong_ports.cpp's
// header for the full map and its MAME citations.
//
// Like ArcadeMachine_Pacman and unlike ArcadeCPU_i8080's extern-global
// read_port()/write_port(), ArcadeCPU_Z80 wires hardware access via
// per-instance function pointers on the z80 struct, so this file exposes a
// "wire" function that assigns them rather than a fixed-name contract the
// CPU core reaches for by symbol.
#ifndef DKONG_PORTS_H
#define DKONG_PORTS_H

#include "dkong_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

// Assigns system->cpu.{read_byte,write_byte,port_in,port_out,userdata}.
// Call once (from dkong_init()) before running the CPU.
void dkong_ports_wire(dkong_system *system);

#ifdef __cplusplus
}
#endif

#endif
