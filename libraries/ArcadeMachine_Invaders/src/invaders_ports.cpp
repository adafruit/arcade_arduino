// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// i8080 port I/O -- identical hardware-facing logic to invaders_pico's
// i8080_ports.c, except play_sound()/stop_sound() (Fruit-Jam-and-earlier
// pico_sound.c calls) become invaders_audio_play()/invaders_audio_stop()
// (this machine's own board-agnostic mixer, see invaders_audio.*).
#include "invaders_ports.h"
#include "invaders_audio.h"

static arcade_system *g_system;

void invaders_ports_bind(arcade_system *system) {
    g_system = system;
}

uint8_t read_port(uint8_t port_number) {
    uint8_t port_data = 0;

    switch (port_number) {
    case 0:
        port_data = (1 - g_system->dip_switches[2])
            | (g_system->dip_switches[4] << 1)
            | (g_system->dip_switches[5] << 2)
            | (g_system->dip_switches[6] << 3)
            | (g_system->shot  << 4)
            | (g_system->left  << 5)
            | (g_system->right << 6);
        break;
    case 1:
        port_data = g_system->coin
            | (g_system->start2 << 1)
            | (g_system->start1 << 2)
            | (1 << 3)
            | (g_system->shot  << 4)
            | (g_system->left  << 5)
            | (g_system->right << 6)
            | (1 << 7);
        break;
    case 2:
        port_data = (1 - g_system->dip_switches[0])
            | ((1 - g_system->dip_switches[1]) << 1)
            | (g_system->tilt << 2)
            | ((1 - g_system->dip_switches[3]) << 3)
            | (g_system->shot  << 4)
            | (g_system->left  << 5)
            | (g_system->right << 6)
            | ((1 - g_system->dip_switches[7]) << 7);
        break;
    case 3:
        port_data = g_system->ext_shift_data >> (8 - g_system->ext_shift_offset);
        break;
    }

    return port_data;
}

void write_port(uint8_t port_number, uint8_t port_data) {
    static uint8_t port_data_mem[2] = {0, 0};

    switch (port_number) {
    case 2:
        g_system->ext_shift_offset = port_data & 0x07;
        break;
    case 3:
        if ( (port_data & 0x01) && !(port_data_mem[0] & 0x01)) invaders_audio_play(0);  // UFO arrives
        if (!(port_data & 0x01) &&  (port_data_mem[0] & 0x01)) invaders_audio_stop(0);  // UFO gone
        if ((port_data & 0x02) && !(port_data_mem[0] & 0x02)) invaders_audio_play(1);   // Player shot
        if ((port_data & 0x04) && !(port_data_mem[0] & 0x04)) invaders_audio_play(2);   // Player killed
        if ((port_data & 0x08) && !(port_data_mem[0] & 0x08)) invaders_audio_play(3);   // Invader hit
        if ((port_data & 0x10) && !(port_data_mem[0] & 0x10)) invaders_audio_play(4);   // Extra ship
        port_data_mem[0] = port_data;
        break;
    case 4:
        g_system->ext_shift_data = (g_system->ext_shift_data >> 8) | (port_data << 8);
        break;
    case 5:
        if ((port_data & 0x01) && !(port_data_mem[1] & 0x01)) invaders_audio_play(5);   // Fleet 1
        if ((port_data & 0x02) && !(port_data_mem[1] & 0x02)) invaders_audio_play(6);   // Fleet 2
        if ((port_data & 0x04) && !(port_data_mem[1] & 0x04)) invaders_audio_play(7);   // Fleet 3
        if ((port_data & 0x08) && !(port_data_mem[1] & 0x08)) invaders_audio_play(8);   // Fleet 4
        if ((port_data & 0x10) && !(port_data_mem[1] & 0x10)) invaders_audio_play(9);   // UFO hit
        g_system->cocktail_vertical_screen_flip = (port_data & 0x20) ? 1 : 0;
        port_data_mem[1] = port_data;
        break;
    case 6:
        break; // Watchdog -- ignored
    }
}
