<!--
SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
SPDX-License-Identifier: MIT
-->

# galaga_host — host-side test harness for ArcadeMachine_Galaga

*(See `../README.md` for what the harnesses are, how they work, and the flags
shared with `pacman_host`. This file covers Galaga-specific detail.)*

Runs the **real** Galaga machine (all 3 Z80 cores, real ROMs, real port decode, real
per-scanline frame interleaving) natively on a development machine, and traces memory/IO
accesses in execution order.

This exists because bringing Galaga up on hardware had a ~5 minute edit/test cycle (75s SWD
flash at 100kHz, plus a power-cycle, plus reconnecting a debugger that tended to wedge the
board mid-DMA). The same run here takes **under a second**, with unlimited tracing and no
hardware in the loop.

```
./build.sh
./galaga_host                       # defaults: 3000 frames, watches the boot-handshake bytes
./galaga_host --frames 800 --watch 6820,6821,6822,6823 --cyc
```

Run it from anywhere — it searches for `galaga_assets/rom/` upward from the working directory,
or takes `--rom DIR` / `$GALAGA_ROM_DIR`.

## Why this works: SAMP's own architecture rule

Every `ArcadeMachine_*` library is board-agnostic by design — machine code talks **only**
through ArcadeHAL, never to a board library. ArcadeHAL is 13 functions. So `hal_host.cpp` is
simply a fourth "board" alongside `ArcadeBoard_FruitJam`, backed by stdio and plain memory
instead of DVI/GPIO/SD, and the entire machine compiles and runs unmodified.

The stub HAL and the `<Arduino.h>`/`<pico.h>` shims live in `../host_common/`, shared with
`pacman_host` so they cannot drift apart — see `../README.md` for the shared setup, the
`--seed-cyc` cycle-wraparound test, and the general rationale.

Video geometry in `hal_host.cpp` is copied from `ArcadeBoard_FruitJam`'s real values
(640x480, 240 scanlines/frame after the 2x vertical repeat) so `run_frame_interleaved()`
slices CPU time with the **same shape** it does on hardware. Keep them in sync.

## The tracer changes no library source

SAMP wires each CPU's memory access through per-instance function pointers on the `z80`
struct (`read_byte`/`write_byte`/`userdata`). After `galaga_init()` has wired them, the
harness captures those pointers and substitutes wrappers that log and then delegate. Nothing
in `ArcadeMachine_Galaga` knows the harness exists — so traces reflect exactly the code that
ships to hardware.

`--watch` takes any addresses, **I/O as readily as RAM**. Watching the misclatch at
`6820`-`6823` is often more informative than watching RAM, since that is where interrupt
enables and the sub/sub2 reset line live.

Output is run-length collapsed (`x1234`). This is essential, not cosmetic: the CPUs sit in
tight poll loops that would otherwise bury real state transitions under millions of identical
lines. To read a trace, filter the known poll loops by PC, e.g.:

```
./galaga_host --frames 760 --watch 9100,9101,9102,92A0 > /tmp/g.log
grep -v "pc=34A7\|pc=34BB\|pc=35F6\|pc=0597\|pc=35F3\|pc=00A5" /tmp/g.log
```

`--stall N` exits with status 3 when N frames pass with no new distinct traced event and no
RAM-test progress — a usable regression check for "does it still boot".

## What it found

The multi-session 3-CPU boot deadlock, in one afternoon, after hardware debugging had stalled:
`dswa` was `0xB3`, but MAME's two `PORT_DIPUNUSED_DIPLOC(mask, IP_ACTIVE_LOW, ...)` switches
default to **set**, making the correct value `0xF7`. With DSWA bit 2 clear, sub's task-`0x0A`
handler (sub ROM `0x0ECA`) skipped its `RET nz` and fell into a routine that reads unmapped
space above its own 4K ROM, XORs two reads of the same address expecting them to differ, and
executes `RST 0` — resetting the sub CPU — every frame. See project memory
(`galaga-port-research.md`, twelfth investigation) for the full chain.

The general lesson, worth applying to the next port: **when an emulated machine hangs, suspect
what the ROM reads from addresses the emulation does not implement faithfully** — unmapped/
open-bus space, unused DIP bits, floating inputs — before suspecting the scheduler.
