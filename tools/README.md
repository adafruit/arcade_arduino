<!--
SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
SPDX-License-Identifier: MIT
-->

# Host test harnesses

Each `*_host/` directory builds one ArcadeMachine_* library into a native
executable that runs the **real** machine code — actual CPU cores, real
ROMs, real port decode, real per-scanline frame interleaving — on your
development machine.

```
host_common/     shared stub ArcadeHAL + <Arduino.h>/<pico.h>/<pico/stdlib.h> shims
galaga_host/     ArcadeMachine_Galaga   (3x Z80)
pacman_host/     ArcadeMachine_Pacman   (1x Z80)
invaders_host/   ArcadeMachine_Invaders (1x i8080)
mspacman_host/   ArcadeMachine_MsPacman (1x Z80, banked/encrypted ROM)
dkong_host/      ArcadeMachine_DKong    (1x Z80 + i8257 DMA + 8035 sound CPU)
btime_host/      ArcadeMachine_BTime    (2x 6502, one of them encrypted)
m6502_test/      ArcadeCPU_M6502 conformance runner -- NOT a machine harness
```

```sh
./galaga_host/build.sh   && ./galaga_host/galaga_host     --frames 5000
./pacman_host/build.sh   && ./pacman_host/pacman_host     --frames 5000
./invaders_host/build.sh && ./invaders_host/invaders_host --frames 5000
./mspacman_host/build.sh && ./mspacman_host/mspacman_host --frames 5000
./dkong_host/build.sh    && ./dkong_host/dkong_host       --frames 5000
./btime_host/build.sh    && ./btime_host/btime_host       --frames 5000 \
                              --rom ../../btime_assets/rom
```

`m6502_test/` is the odd one out: it runs only the CPU core, against the
standard 6502 test suites (Klaus Dormann's functional test, Bruce Clark's
decimal test, AllSuiteA), and reports cycle counts against upstream's
published figures. Burger Time is this project's first 6502 machine, and a
CPU bug does not present as a CPU bug -- it presents as wrong graphics, a
hang, or a game that boots and then misbehaves. Removing the interpreter
from the suspect list costs one command. Its test binaries are third-party
and fetched rather than vendored; see its `main.cpp` header.

Each harness searches upward for its own `*_assets/` directory, or takes an
explicit path (`--rom DIR` for the two Namco games, `--assets DIR` for
Invaders, which needs both `rom/` and `samples/` — its machine layer loads
WAV samples off storage and `invaders_load_assets()` fails outright if none
load, exactly as it shows the yellow boot-error screen on hardware).

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

The shims in `host_common/shim/` cover the only places a machine library
reaches outside that contract: `<Arduino.h>` for DEBUG `Serial.print`
instrumentation, and `<pico.h>` for `__not_in_flash_func()` in the audio
files (a deliberate, documented exception — see DEVNOTES.md problem #7).
`<pico/stdlib.h>` was added for the Invaders harness and is a CPU-library
need, not a machine-library one: `ArcadeCPU_i8080`'s `i8080.c` includes it
for `tight_loop_contents()` in a `cpu_panic()` that is currently
unreachable (undocumented opcodes and `HLT` are 4-cycle NOPs instead).
`ArcadeCPU_Z80` needs no equivalent.

**If a new machine library needs more than those shims, that is a signal
worth heeding**: it means board-specific code has leaked into the machine
layer, and the fix belongs in the library, not here.

`hal_host.cpp`'s storage stub also implements a real `hal_storage_list_dir()`
(it used to return `false`, since both Namco games load by explicit
manifest). The two 8080bw games discover their ROM chips by **listing**
`/rom` and sorting the names reverse-alphabetically — that sort *is* their
address mapping, so a harness for those games cannot work without it. It
deliberately does not filter dotfiles: `invaders_assets.cpp` does its own
filtering for a documented reason (macOS AppleDouble sidecars on FAT32), and
hiding them here would hide a regression in that filter.

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

### `--digest-every`: when rendered output is *supposed* to change

Rendered-frame comparison is the wrong instrument for one specific and
recurring class of change: **interleaving CPU execution with scanline
submission** (DEVNOTES.md problems #20, #34, #36). That change deliberately
makes each scanline reflect mid-frame VRAM instead of the frame's final
state — as real scanline-order CRT hardware does — so the picture legitimately
differs and a PPM `cmp` reports a failure that is not one.

What must *not* change is which instructions run and when the per-frame
interrupts fire. `invaders_host --digest-every N` prints an FNV-1a hash of
the whole emulated machine (registers, SP, PC, condition codes,
interrupt-enable, the full 64K address space including VRAM, and the
external shift register), which measures exactly that:

```sh
# build a second binary from a copy of the library at the previous revision
MACHINE_SRC=/tmp/old/src OUT=/tmp/invaders_host_old ./invaders_host/build.sh

for b in ./invaders_host/invaders_host /tmp/invaders_host_old; do
  $b --frames 4000 --digest-every 25 --press-frames 12 \
     --input 100:coin,160:start1,400:shoot,600:left,900:right,1200:shoot \
     | tail -n +3 > "$(basename $b).log"
done
diff invaders_host.log invaders_host_old.log && echo IDENTICAL
```

**Run the negative control too.** A digest that never differs proves
nothing, and this project has already paid once for a comparison among
answers that were all wrong (#32). Moving the *coin* press by one frame
diverges the digest at that frame and stays diverged; moving a `shoot` press
by one frame during attract mode changes nothing at all, because there is no
game running to shoot in. If your control does not diverge, fix the control
before trusting the result.

### `--banks`: measuring hardware you cannot see

Ms. Pac-Man's aux daughterboard — the encrypted second ROM bank and the
eight address ranges that switch to it — is the entire difference between
that machine and Pac-Man, and it is nearly invisible from outside. If the
decode is wrong or the bank switching never fires, the likely outcome is not
a crash: it is **plain Pac-Man**, or Pac-Man with forty 8-byte holes punched
through it. Both look like a working port to anyone glancing at an attract
screen.

`mspacman_host --banks` turns that into a number. It counts bank switches,
attributes each to the trigger range that caused it, and reports how many
frames ended in each bank:

```sh
./mspacman_host/mspacman_host --frames 1600 --banks \
    --input 600:coin,800:start1 --press-frames 10
```
```
  total switches: 4   current bank: 1 (decrypted/Ms.)
  frames ending in: decrypted=1102  plain=498
    0038 dis : 2
    3ff8 ENA : 2
```

Zero switches means the aux board is inert and you are running Pac-Man. The
harness keeps its **own** copy of the trigger-range list rather than sharing
one with `mspacman_ports.cpp`, on purpose: if the two ever disagree,
switches get attributed to `other` and say so loudly, which beats a harness
that agrees with the bug it is supposed to find.

It costs nothing in the library. ArcadeCPU_Z80 wires memory access through
per-instance function pointers on the `z80` struct, so the harness captures
those after init and substitutes counting wrappers that delegate to the
originals — the same trick `galaga_host --watch` uses, and the reason
neither needs a single `#ifdef` in machine code.

### `--dma`: the other instrument for hardware you cannot see

Same idea as `--banks`, different machine. Donkey Kong's sprites reach the
video hardware only through an i8257 DMA controller, and if that emulation
is wrong the screen shows a perfectly good background tilemap with no
Mario, no barrels and no Kong. That reads as "the renderer is broken" and
sends you into the wrong file.

```sh
./dkong_host/dkong_host --frames 1200 --dma
```
```
  8257 transfers: 1193  bytes moved: 459305
  peak sprites on one scanline: 8   16-limit hit: 0 times
```

Roughly one transfer per frame of ~384 bytes is healthy. Zero transfers is
called out explicitly in the output, because that is the case worth naming
rather than leaving to be inferred from a small number.

The sprite counters are there for a second reason: this hardware buffers one
scanline into a 64x9 line RAM, which limits it to **16 sprites per
scanline**, and the game relies on that. The port emulates the limit rather
than ignoring it, so `16-limit hit` going up is correct behaviour under
load, not a warning.

### Judging sound you cannot simulate

`dkong_host` carries four sound flags, and each exists because a different
question could not be answered any other way:

- `--wav FILE` captures the machine's own fill callback to a 16-bit WAV.
  Donkey Kong's discrete channels are approximations tuned by ear against
  recordings, exactly as Galaga's 54XX explosion was, and an approximation
  can only be judged by listening.
- `--audio` reports sound-CPU activity and audio-FIFO health. Zero cycles,
  or a FIFO that under-runs, are conditions worth naming rather than
  inferring from a WAV that sounds wrong.
- `--sndtrace N` dumps the sound CPU's instruction stream. "Is the CPU
  executing the code I think it is" is a question about the CPU; three
  rounds of poking at audio output failed to answer it and one trace did.
- `--channels M` solos individual channels (bit0 DAC/music, bit1 stomp,
  bit2 jump, bit3 walk). Once a channel is mixed at the RIGHT level it
  disappears under the music in a spectrum, and its own verification becomes
  impossible. "I cannot see it" is not "it is not there".

### PPM dumps and what the monitor actually shows

**The real canvas is 320x240 square pixels, not 640x480.** `HAL_VIDEO_WIDTH`
is 640, but only the **first 320** pixels of each scanline buffer are ever
displayed: the vendored libdvi's 16bpp path encodes `h_active_pixels / 2`
source pixels across the full line (`_dvi_prepare_scanline_16bpp()` in
`PicoDVI - Adafruit Fork/src/libdvi/dvi.c`), doubling horizontally exactly as
`dvi_vertical_repeat = 2` doubles vertically. That is why every renderer in
this project lays its picture out against a 320-wide visible axis — see the
`TATE_BX`/`LAND_BX` constants, all written as `(320 - N) / 2`.

All six harnesses now share one dumper, `host_common/host_ppm.cpp`, which
reproduces **both** doublings. Output stays 640x480 and looks like the
monitor. Two properties hold of every dump and are worth asserting on if you
ever doubt one: adjacent output rows are identical in pairs, and adjacent
output columns are identical in pairs.

**Why this replaced six private copies.** Every harness's old `dump_ppm()`
looped `y = 0 .. HAL_VIDEO_HEIGHT-1`, rendering **480** rows. The device
submits **240** and lets the hardware repeat each one (DEVNOTES #10). For
tate that difference is invisible — tate's formula halves `dvi_y` and lands
on the same native row either way. For **landscape** it is the entire bug:
`dy = dvi_y * GAME_WIDTH / HAL_VIDEO_HEIGHT` is a resampling ratio, and

```
480 samples (old dumper):  288 of 288 source columns drawn,   0 dropped
240 samples (real device): 240 of 288 source columns drawn,  48 dropped
```

So the harnesses rendered landscape at the **pre-#10 sample rate** — they
drew it the lossless way and structurally could not reproduce the defect.
Anything validated against those dumps was validated against a model that
did not contain the bug. A wrong measuring instrument is worse than a wrong
subject: it does not look like a defect, it looks like evidence. See
DEVNOTES #75/#76 and `DISPLAY_GEOMETRY.md`.

**This changes every stored byte-compare baseline** — regenerate the
`before_*.ppm` files any workflow keeps. `galaga_host` and `pacman_host`
move most: their old dumps did not horizontal-double at all, so the picture
sat in the left half against black.

Use `--rotation N` (all six harnesses have it) to dump an orientation
without hardware:

```sh
./pacman_host/pacman_host --rotation 0 --frames 3100 --ppm-every 1500 --ppm-prefix land
```

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
