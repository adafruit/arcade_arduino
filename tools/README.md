# Host test harnesses

Each `*_host/` directory builds one ArcadeMachine_* library into a native
executable that runs the **real** machine code — actual CPU cores, real
ROMs, real port decode, real per-scanline frame interleaving — on your
development machine.

```
host_common/     shared stub ArcadeHAL + <Arduino.h>/<pico.h> shims
galaga_host/     ArcadeMachine_Galaga   (3x Z80)
pacman_host/     ArcadeMachine_Pacman   (1x Z80)
```

```sh
./galaga_host/build.sh && ./galaga_host/galaga_host --frames 5000
./pacman_host/build.sh && ./pacman_host/pacman_host --frames 5000
```

Each harness searches upward for its own `*_assets/rom/` directory, or
takes `--rom DIR`.

## Why these exist

A hardware iteration costs about **3 minutes** — a 75-second SWD flash at
100kHz, a power-cycle, then watching the screen. The same test here takes
about **a second**, with unlimited tracing, no debugger wedging the board,
and no camera pointed at a monitor.

They are not a substitute for hardware testing. They cannot tell you
anything about DVI timing, the audio DAC, GPIO, or the frame budget. They
*are* the fastest way to answer "what is the emulated machine actually
doing", which is where most of the hard bugs turned out to live.

## Why this works at all

SAMP's architecture rule does the heavy lifting: every `ArcadeMachine_*`
library is board-agnostic and talks **only** through ArcadeHAL — 13
functions. So `host_common/hal_host.cpp` is simply a fourth "board"
alongside `ArcadeBoard_FruitJam`, backed by stdio and plain memory instead
of DVI/GPIO/SD, and an entire machine compiles and runs unmodified.

The two shims in `host_common/shim/` cover the only places a machine
library reaches outside that contract: `<Arduino.h>` for DEBUG
`Serial.print` instrumentation, and `<pico.h>` for `__not_in_flash_func()`
in the audio files (a deliberate, documented exception — see DEVNOTES.md
problem #7).

**If a new machine library needs more than those shims, that is a signal
worth heeding**: it means board-specific code has leaked into the machine
layer, and the fix belongs in the library, not here.

## `--seed-cyc`: testing cycle-counter wraparound in seconds

The Z80's cycle counter is 32-bit and never reset, so at 3.072MHz it wraps
roughly every **23 minutes** of runtime. Both machines' frame loops are
written to survive that (unsigned elapsed-delta subtraction), and
DEVNOTES.md problem #22 records a real permanent hang caused by getting it
wrong. Nobody tests it, because nobody wants to sit in front of a cabinet
for 23 minutes.

`--seed-cyc N` sets the counter(s) right after init and before any cycles
run, so the wrap arrives within a few frames:

```sh
# wraps ~3 frames in, then runs 4000 frames through and past it
./pacman_host/pacman_host --seed-cyc 4294800000 --frames 4000 --stall 3 --ppm-every 1000
./galaga_host/galaga_host --seed-cyc 4294800000 --frames 8000 --stall 400
```

Seeding happens *before* boot deliberately, so anything the machine derives
from absolute cycle values (Galaga's `namco_busy_until`, `io06_nmi_next`,
`reset_release_main_cyc`) is computed relative to the seeded base.

**This class of bug is worth taking seriously.** Galaga froze after ~12
minutes on hardware because a signed 32-bit difference overflowed, and the
harness could not reproduce it at first: `cyc` was `unsigned long`, which is
32-bit on the device but **64-bit on a typical host**. It is now pinned to
`uint32_t` in `z80.h` so host and device agree. The related trap: the
wraparound idiom `(long)(a - b) < 0` is only correct when `long` matches the
counter width — **cast to the exact width (`int32_t`), never to `long`.**

## Verifying a change did not alter behaviour

`--ppm-every N` renders whole frames through the real renderer to PPM. The
standard regression check for an optimisation is that the rendered output
stays byte-identical:

```sh
./galaga_host/galaga_host --frames 12800 --quiet --ppm-every 12720 --ppm-prefix after
cmp after_12720.ppm before_12720.ppm && echo IDENTICAL
```

Every performance change in the Galaga port (interleave quantum, per-frame
sprite decode, RAM-resident code, render fast path, pen LUTs) was validated
this way before flashing.

## Counting what you actually care about

`--census A B` prints the on-screen sprite list for frames A..B;
`--census-code C` narrows it to one sprite code and prints a count and
positions. Galaga's player bullet is code `0x30`:

```sh
./galaga_host/galaga_host --input 1200:coin,1400:start1,2000:fire \
    --press-frames 6 --census 2000 2014 --census-code 0x30
```

**This flag exists because of a mistake worth not repeating.** Galaga's
"every press fires two shots" bug was investigated by comparing rendered
frames across presses of 1, 10, 30, 60, 90 and 110 frames. All six came out
byte-identical, which was read as "press length has no effect, so the
emulation is fine". All six were firing **two** bullets — it was a
comparison among six wrong answers, which agreed with each other perfectly,
and it sent a whole session chasing a hardware fault that did not exist (see
DEVNOTES.md #32).

Byte-identical PPM comparison is the right tool for "did this optimisation
change behaviour", where you already know the *before* was correct. It is
the wrong tool for "is this behaviour correct". For that, count the thing:
the bullet count showed the bug in a single run.

## Other flags

- `--input SPEC` — scripted button presses, e.g. `1200:coin,1400:start1,2400:right`.
  Enough to coin up, start a game, move and shoot with no hands and no hardware.
- `--watch a,b,c` (galaga_host) — trace reads/writes of specific addresses,
  RAM **or I/O**, with PC, CPU and cycle counts, run-length collapsed.
  Watching the misclatch at `6820`-`6823` was what cracked the boot deadlock.
- `--stall N` — exit status 3 if nothing changes for N checks, so a harness
  run can be used as a pass/fail regression test in a script. The default
  (180) is aggressive; pass `--stall 0` for long runs that legitimately sit
  still, such as an attract loop.
- `--wav FILE` — captures the machine's own audio fill callback (the same
  one the board's audio ISR drives) to a 16-bit WAV, so synthesis can be
  *listened to* before flashing. This is how Galaga's 54XX explosion was
  tuned against a recording of a real board.
- `--ppm-from N` / `--ppm-to N` — bound the PPM dump window. With
  `--ppm-every 1` this captures a run of consecutive frames deep into a
  session without writing ~1MB per frame for everything before it; it is how
  the starfield's 1px/frame scroll rate was measured.
