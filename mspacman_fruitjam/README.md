<!--
SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
SPDX-License-Identifier: MIT
-->

# mspacman_fruitjam

Ms. Pac-Man on the Adafruit Fruit Jam — see the [top-level README](../README.md)
for the overall SAMP framework and general build steps. This page covers
what's specific to this game.

## What makes this one different

Ms. Pac-Man is not its own PCB. It is a **stock Pac-Man board with a
daughterboard** — the "Ms. Pac-Man auxiliary board" — piggybacked onto the
Z80 socket, carrying three extra ROMs (`u5`/`u6`/`u7`) and an address
decoder. Video, audio, input, the PROMs, the tile/sprite decode and the
frame timing are all bit-identical to Pac-Man's.

`ArcadeMachine_MsPacman` is therefore a **sibling** of
`ArcadeMachine_Pacman`, not a variant of it: most of its files are copies,
and everything Ms. Pac-Man-specific is confined to two of them.

**`mspacman_assets.cpp` — the ROM decode.** The aux ROMs are scrambled on
both their address and data lines. At load time the machine builds a second,
*decrypted* 48K ROM bank out of them, then copies **forty 8-byte patches**
from the decrypted `u5` image over the Pac-Man code beneath it. That is what
turns Pac-Man's program into Ms. Pac-Man's. Transcribed from MAME's
`pacman_state::init_mspacman()` and `mspacman_install_patches()`.

**`mspacman_ports.cpp` — the banked bus.** The CPU sees one of two banks at
a time. Eight address ranges flip the selection **on any access, read or
write**, with a read still returning a byte — from the bank being switched
*to*, not the one that was live. Seven of them select the plain Pac-Man bank
and exactly one (`0x3FF8-0x3FFF`) selects the decrypted one; that asymmetry
is real. RAM and I/O are also mirrored with a `0xA000` mask here, unlike
plain Pac-Man, because ROM now occupies `0x8000-0xBFFF` and the high quarter
of the address space is where this game's RAM lives from the CPU's point of
view. Transcribed from MAME's `pacman_state::mspacman_map()`.

The board powers up with the **decrypted** bank selected
(`init_mspacman()`'s closing `set_entry(1)`).

## Required assets

An SD card, FAT32-formatted with an **MBR** partition scheme (not
GPT/exFAT — macOS Disk Utility defaults to GPT on "Erase"), containing your
own legally-obtained Ms. Pac-Man ROM/PROM set:

```
/rom/
    pacman.6e   pacman.6f   pacman.6h   pacman.6j     <- Pac-Man program ROMs
    u5          u6          u7                        <- aux board ROMs
    5e          5f                                    <- graphics ROMs
    82s123.7f   82s126.4a   82s126.1m                 <- colour + waveform PROMs
```

Two filename traps, both of which fail quietly rather than loudly:

- **The graphics ROMs are `5e`/`5f`, not `pacman.5e`/`pacman.5f`.** Those
  are different dumps with different CRCs — this set's are `5c281d01` and
  `615af909`. A missing gfx ROM is deliberately *not* a boot error (see
  `mspacman_assets.cpp`), so the wrong names give you a running game with a
  garbled character set.
- **`pacman.6e/6f/6h/6j` are byte-identical to plain Pac-Man's.** A
  directory holding only those will look plausible and is not a usable
  Ms. Pac-Man set. The presence of `u5` is what distinguishes them, which is
  what the host harness probes for.

`82s126.3m` is the "Timing" PROM MAME's own driver comments as *not used*;
it is never loaded, and its absence is harmless.

The seven program ROMs (including `u5`/`u6`/`u7`) are all **required** — a
missing one gives a boot-error screen rather than a subtly broken game,
which is the intent.

## Controls

| Button | Action |
|---|---|
| COIN | Insert coin |
| START1 / START2 | 1-player / 2-player start |
| UP / DOWN / LEFT / RIGHT | 4-way joystick |
| ROTATE | Cycle screen rotation (0°/90° CCW "tate"/180°/270° CW) |
| MIRROR | Toggle horizontal mirror (for Pepper's-Ghost half-silvered-mirror cabinets) |
| STRETCH (Button 1) | Toggle aspect-ratio correction — **on by default on this game**, because its raster is already close to 4:3 so the correction is nearly free and nearly invisible (+3.7%). Press to turn it off. Which setting looks right depends on your MONITOR, not the game: a 16:9 panel already stretches a rotated picture on its own, while a panel forced to 4:3 — or a real 4:3 panel — does not. Try both and keep the one that looks correct. |

Identical to `pacman_fruitjam`'s: a Ms. Pac-Man cabinet is the same 4-way
joystick with no action button, so `HAL_BTN_SHOOT` is unused.

Default rotation is **3** (90° CW), same as Pac-Man — the Namco cabinets
mounted their monitors opposite to the 8080bw games, so this differs from
Invaders/Lunar Rescue on purpose. See `DEVNOTES.md` #33.

## Notes

RAM use is the highest of any game in the project — **63%** of the RP2350's
512KB, against Pac-Man's 47%. The extra 80KB is the second ROM bank: two
banks of 48KB each, replacing Pac-Man's single 16KB `rom[]`. Storing a full
0xC000 per bank wastes the unused 0x4000-0x7FFF window in each, and that is
a deliberate trade — it keeps every index in the decode identical to the
MAME source it is transcribed from, and a transposed index there produces a
game that runs the wrong code rather than anything a compiler could catch.

## Testing without hardware

`../tools/mspacman_host/` builds this game's machine library — the real Z80
core, real ROMs, the real aux-board decode and bank switching — into a
native executable. See `../tools/README.md`.

```sh
./../tools/mspacman_host/build.sh
../tools/mspacman_host/mspacman_host --frames 2000 --banks \
    --input 600:coin,800:start1,1000:left --press-frames 60 \
    --ppm-every 1500 --ppm-prefix ms
```

`--banks` is specific to this harness and exists because the aux board is
almost invisible from outside: if the decode or the bank switching is wrong,
the likely outcome is not a crash but **plain Pac-Man**, which looks like a
working port at a glance. It counts the switches and attributes them to
individual trigger ranges. A healthy short attract-mode run shows a handful
of switches across both the `3ff8 ENA` and one or more `dis` ranges; zero
switches means the aux board is inert.
