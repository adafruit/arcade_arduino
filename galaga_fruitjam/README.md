<!--
SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
SPDX-License-Identifier: MIT
-->

# galaga_fruitjam

Galaga on the Adafruit Fruit Jam — see the [top-level README](../README.md)
for the overall SAMP framework and general build steps. This page covers
what's specific to this game.

This is the project's **first multi-CPU port** (`ArcadeMachine_Galaga`,
built on `ArcadeCPU_Z80`) — real Galaga hardware is 3 shared-RAM Z80s plus
a custom Namco 06XX/51XX/54XX I/O chain and a 4th video layer
(procedurally-generated starfield). Every hardware fact this library
relies on (memory map, interrupt/NMI scheme, CPU interleave quantum,
custom I/O protocol, tile/sprite/palette decode) was verified directly
against [MAME](https://github.com/mamedev/mame)'s own `galaga` driver,
device, and video source (`src/mame/namco/galaga.cpp`, `galaga_v.cpp`,
`namco06.cpp`, `namco51.cpp`, `namco54.cpp`) — see
`ArcadeMachine_Galaga/src/*.cpp`'s own header comments for exact
citations, same rigor `ArcadeMachine_Pacman`'s README applies to its own
sources.

## Scope -- what's approximated

All four layers of the game are present: the three video layers (05XX
starfield, sprites, tilemap), all three Z80s, the full input path, the
Namco WSG's three voices and the 54XX explosion channel. What follows is
where this port knowingly approximates the hardware rather than
reproducing it:

- **The 54XX explosion channel is synthesized, not ported.** MAME emulates
  this chip at low level, running its real Fujitsu MB8844 firmware, which
  this project has no dump of. The command protocol and the three analog
  filter bands are taken from cited MAME sources (`namco54.cpp`'s protocol
  table, `galaga_a.cpp`'s discrete netlist component values); the noise
  generator and envelope shape are not, and were tuned by ear against a
  recording of a real board. See `galaga_54xx.h`.
- **The starfield does not clock its LFSR per pixel.** MAME's
  `starfield_05xx.cpp` steps the shift register once per pixel, ~65536
  times a frame, which does not fit this board's frame budget. Because the
  sequence is fixed, the 256 star positions are instead precomputed once
  and only re-bucketed per frame. The output is identical; see
  `galaga_video.cpp` for the derivation and for the exhaustive enumeration
  that pins down the table bounds.
- **Coin/credit bookkeeping is a simplification.** The 51XX custom chip
  (coin/credit/joystick I/O) is hand-coded high-level emulation of its
  documented command protocol, not a port of the real Fujitsu MB8843
  firmware (this project has no dump of it) -- see
  `ArcadeMachine_Galaga/src/galaga_51xx.h` for exactly what is and isn't
  modeled.
- **3-CPU interleave granularity is an approximation**, not a byte-exact
  reproduction of MAME's scheduler -- see `galaga_machine.cpp`'s header
  comment. If gameplay logic misbehaves (garbled shared-RAM handshakes
  between the 3 CPUs), this is the first thing to revisit.

Tiles, sprites (including Galaga's variable-size 1x1/1x2/2x1/2x2-cell
sprites — a real hardware capability Pac-Man's fixed-16x16 sprites don't
have), the scrolling starfield, and 2-way-joystick + fire + coin/start
input are real, not placeholders.

## Required assets

An SD card, FAT32-formatted with an **MBR** partition scheme (not
GPT/exFAT — macOS Disk Utility defaults to GPT on "Erase"):

```
/rom/
    gg1-1b.3p
    gg1-2b.3m
    gg1-3.2m
    gg1-4b.2l
    gg1-5b.3f
    gg1-7b.2c
    gg1-9.4l
    gg1-11.4d
    gg1-10.4f
    prom-5.5n
    prom-4.2n
    prom-3.1c
    prom-1.1d
```

This is MAME's `galaga` ROM set ("Galaga, Namco rev. B") — SHA1-verified
against the actual MAME source this session, not just filename-matched.
`prom-1.1d` is the Namco WSG's waveform table — add it, or the game boots
and plays normally but is silent (its absence is non-fatal by design, like
the colour PROMs). `prom-2.5c` (a timing PROM) is part of the
full real-hardware ROM set but is not loaded here; fine to leave on the
card, it is simply unused.

## Controls

| Button | Action |
|---|---|
| COIN | Insert coin |
| START1 / START2 | 1-player / 2-player start |
| LEFT / RIGHT | Move (2-way joystick — Galaga has no up/down) |
| SHOOT | Fire |
| ROTATE | Cycle screen rotation (0°/90° CCW "tate"/180°/270° CW) |
| MIRROR | Toggle horizontal mirror (for Pepper's-Ghost half-silvered-mirror cabinets) |
| STRETCH (Button 1) | Toggle aspect-ratio correction — **tate only on this game.** Galaga is the most expensive machine in the project (three Z80s), and in landscape the correction costs +2,576us a frame, which puts it past the whole frame budget and fills the screen with red. So landscape is pinned to the uncorrected 1:1 layout and this button does nothing in rotations 0 and 2. See `DEVNOTES.md` #101. In tate it works and is worth having, though it leaves less headroom than any other game/rotation here. |

Galaga's native hardware framebuffer (288x224, before the cabinet's
physical 90-degree mount) is displayed **portrait**, defaulting to rotation
**3** (90° CW). Note that is deliberately *not* the same value Space
Invaders and Lunar Rescue default to (1): the Namco and 8080bw cabinets
mounted their monitors in opposite orientations, so the two families need
opposite software rotations to come up upright on one physical screen. All
four games in this project are therefore upright together on the same
monitor without touching the ROTATE button — see `galaga_machine.cpp`'s
comment at the default for the full reasoning.

## Notes

Verified on real Fruit Jam hardware: boots, passes the power-on self-test
(RAM march → grid → colour sweep), and **plays** — starfield, sprites,
joystick/fire, WSG music and the 54XX explosion — at a flat 60fps with
roughly 3ms of frame-budget headroom, indefinitely. Bringing it up turned up
five real bugs worth knowing about, all recorded in `../DEVNOTES.md` #24-32
and in comments at the relevant code:

- **Two DIP-switch bits** (`dswa` 0x04/0x40) were left clear. MAME's
  `PORT_DIPUNUSED_DIPLOC(mask, IP_ACTIVE_LOW, …)` defaults them to **set** —
  "unused" describes the switch, not what the CPU reads. With bit 2 clear,
  sub CPU's task-0x0A handler executed `RST 0` (a self-reset) every frame
  and the 3-CPU boot handshake never completed. See `galaga_assets.cpp`.
- **Frame-budget overruns** showed as partial/full red screens (a starved
  DVI scanline queue, not a rendering fault). Fixed by interleave-quantum,
  per-frame sprite decode, a render fast path, pen LUTs, and — the big one —
  moving the Z80 interpreter and hot paths into SRAM (`.time_critical`),
  since a switch-based interpreter thrashes the RP2350's XIP flash cache.
- **A 32-bit signed overflow** froze the game after ~12 minutes. See
  `galaga_machine.cpp`'s `interleave_to_target()`.
- **The 54XX explosion** was sized from the reference recording's *length*
  rather than its decay *slope*, and its Q16 envelope truncated away the
  tail about four times early. It also needed a sample-and-hold on the noise
  (the real chip is an MCU and cannot update its DAC per audio sample) and
  had to layer both sound types, which Galaga fires together.
- **Fire was reported as a level when the hardware pulses it.** Galaga's ROM
  fires one bullet per frame it reads the bit set, so every press fired two
  shots. Now a read-confirmed one-shot pulse — see `galaga_51xx.cpp`.

## Known issue — a red line under extreme load

With an entire enemy formation on screen *and* the player firing or being
destroyed, a brief red line can appear. Normal play rarely reaches this —
most players shoot the formation down as it assembles — which is why it went
unseen through all earlier testing.

It is **not** the frame budget (peak `work` is 14946 µs of 16660, never
over), not sprite count alone (64 sprites with no red line), not the audio
buffer size (reproduced at 256, 128 and 64), and not the optimisation level
(already `-O3`). See `../DEVNOTES.md` problem #35 for the full list of what
has been ruled out, two instruments that gave misleading answers, and the
best remaining hypothesis — Galaga's audio ISR, the only one in this project
never instrumented, and the heaviest per sample.

The instruments needed for that work are already in place: `work`/`blocked`
and `work_MAX`/`sprites_max` in the sketch, and
`galaga_debug_take_starvation()` in `galaga_machine.cpp`.

Most of that was diagnosed on the host harness
(`../tools/galaga_host`), which runs this exact machine code natively in
under a second per test rather than minutes per hardware flash. Use it
first — but read `../DEVNOTES.md` #32 on how to use it without fooling
yourself, since the fire bug was *hidden* by a harness test that compared
several equally-wrong runs against each other.
