// i8080 port I/O -- every bit assignment below is transcribed directly from
// MAME's midw8080 driver, not inferred by analogy to Space Invaders:
//   - IN0/IN1/IN2 read layout: midw8080/8080bw.cpp's sicv_base INPUT_PORTS
//     (lrescue includes it, only overriding two DIP bits -- see case 2
//     below) plus midw8080/mw8080bw.cpp's invaders_in1_control_r()/
//     invaders_in2_control_r() (lrescue reuses Space Invaders' own control-
//     read helpers).
//   - Port 2/3/4 shift register wiring: midw8080/8080bw.cpp's
//     lrescue_io_map (identical mb14241 wiring to Space Invaders' io_map).
//   - Port 3/5 sound-trigger bits: midw8080/8080bw_a.cpp's
//     lrescue_sh_port_1_w()/lrescue_sh_port_2_w().
#include "lrescue_ports.h"
#include "lrescue_audio.h"

static arcade_system *g_system;

void lrescue_ports_bind(arcade_system *system) {
    g_system = system;
}

uint8_t read_port(uint8_t port_number) {
    uint8_t port_data = 0;

    switch (port_number) {
    case 0:
        // sicv_base's IN0 carries no player controls (unlike Space
        // Invaders' own IN0, which duplicates P1 controls here) -- every
        // bit is either an unused/tied-high line or an IPT_UNKNOWN DIP this
        // driver never assigns a meaning to. Lunar Rescue's code should
        // never depend on this port's value.
        port_data = 0xFF;
        break;
    case 1:
        // Bit-for-bit identical to Space Invaders' own IN1 (same
        // PORT_BIT layout in sicv_base as in mw8080bw.cpp's INPUT_PORTS_START(invaders)).
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
        // Same bit positions as Space Invaders' own IN2 (lives @0-1, tilt
        // @2, bonus-life @3, P1/P2 control @4-6, coin-info @7). Lunar
        // Rescue's own INPUT_PORTS_START(lrescue) marks bits 3 and 7
        // DIPUNUSED/factory-fixed rather than user-adjustable -- see
        // lrescue_assets.cpp's dip_switches[] defaults, which bake in the
        // fixed values those bits need.
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
        // mb14241 shift-register result -- identical wiring to Space
        // Invaders (lrescue_io_map maps this the same way as invaders'
        // own io_map).
        port_data = g_system->ext_shift_data >> (8 - g_system->ext_shift_offset);
        break;
    }

    return port_data;
}

void write_port(uint8_t port_number, uint8_t port_data) {
    static uint8_t port_data_mem[2] = {0, 0};

    switch (port_number) {
    case 2:
        // shift_count_w -- identical wiring to Space Invaders.
        g_system->ext_shift_offset = port_data & 0x07;
        break;
    case 3:
        // lrescue_sh_port_1_w(): rising-edge sample triggers, plus two
        // continuous (not edge-triggered) bits read every write.
        {
            uint8_t rising = port_data & ~port_data_mem[0];
            if (rising & 0x01) lrescue_audio_play(LRESCUE_SND_THRUST);
            if (rising & 0x02) lrescue_audio_play(LRESCUE_SND_BEAMGUN);              // player shot
            if (rising & 0x04) lrescue_audio_play(LRESCUE_SND_RESCUESHIP_EXPLOSION); // player death
            if (rising & 0x08) lrescue_audio_play(LRESCUE_SND_ALIEN_EXPLOSION);      // alien hit
            if (rising & 0x10) lrescue_audio_play(LRESCUE_SND_BONUS3);               // bonus ship (MAME: "not confirmed")

            g_system->screen_red = (port_data & 0x04) != 0;
            lrescue_audio_set_mute(!(port_data & 0x20));

            port_data_mem[0] = port_data;
        }
        break;
    case 4:
        // shift_data_w -- identical wiring to Space Invaders.
        g_system->ext_shift_data = (g_system->ext_shift_data >> 8) | (port_data << 8);
        break;
    case 5:
        // lrescue_sh_port_2_w(): rising-edge sample triggers, a loop
        // start/stop pair (same start-on-rising/stop-on-falling pattern as
        // Invaders' UFO sound on port 3 bit 0), and the one genuinely
        // synthesized channel -- a bare 1-bit speaker level (MAME:
        // SPEAKER_SOUND device) used for two "bitstream" jingles. There is
        // no sample for this; lrescue_audio_speaker_event() timestamps
        // this transition against system->total_cycles so the mixer can
        // reconstruct the waveform in real time later, rather than just
        // recording "the current level" -- see that function's doc
        // comment for why a live-polled level can't reproduce a tune.
        {
            uint8_t rising  = port_data & ~port_data_mem[1];
            uint8_t falling = ~port_data & port_data_mem[1];

            if (rising & 0x01) lrescue_audio_play(LRESCUE_SND_STEPH); // footstep high tone
            if (rising & 0x02) lrescue_audio_play(LRESCUE_SND_STEPL); // footstep low tone
            if (rising & 0x04) lrescue_audio_play(LRESCUE_SND_BONUS2); // bonus (counting rescued men)

            // g_system->total_cycles -- deliberately NOT a real-time clock
            // like lrescue_audio_now_cycles(). An earlier version of this
            // line used that instead, reasoning that total_cycles' rate had
            // been measured drifting from real time (see
            // lrescue_machine.h's doc comment on total_cycles for the full
            // story) -- true, but switching the *timestamp domain* was the
            // wrong fix: Core 0 races through a whole frame's instructions
            // in ~2ms of real time, then blocks on the video queue for the
            // rest of that frame's real ~16.7ms. total_cycles increments
            // smoothly across that 2ms burst, spreading each write's
            // *logical* position evenly across the frame's cycle budget --
            // which is exactly what lets the audio ISR's real-time-paced
            // target_cycle stretch them back out correctly on the other
            // end. Timestamping with real wall-clock time instead collapses
            // a whole frame's writes to nearly the same instant, undoing
            // that stretch and reproducing the exact "phrase happens all at
            // once" failure this whole scheme exists to prevent (heard as
            // a buzz-then-silence pattern once per frame). The actual fix
            // for total_cycles' drift is calibrating FRAMERATE, not
            // changing which clock domain this call uses.
            lrescue_audio_speaker_event(g_system->total_cycles, (port_data & 0x08) != 0);

            // Ruled out as the cause of either symptom by direct testing
            // (temporarily skipping this pair changed neither the red
            // line nor the audio quality) -- restored to normal.
            if (rising  & 0x10) lrescue_audio_play(LRESCUE_SND_SHOOTINGSTAR); // loops
            if (falling & 0x10) lrescue_audio_stop(LRESCUE_SND_SHOOTINGSTAR);

            g_system->flip_screen = (port_data & 0x20) ? 1 : 0; // cocktail-only on real hardware; unused by the renderer

            port_data_mem[1] = port_data;
        }
        break;
    case 6:
        break; // Watchdog -- ignored
    }
}
