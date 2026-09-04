<!--
SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
SPDX-License-Identifier: MIT
-->

# btime_fruitjam

**Burger Time** (Data East, 1982) on the Adafruit Fruit Jam —
`ArcadeMachine_BTime` + `ArcadeCPU_M6502` + `ArcadeBoard_FruitJam`.

Built from scratch against the real `btime` ROM set and verified against
MAME's `src/mame/dataeast/btime.cpp` and `decocpu7.cpp` driver sources, not
ported from any existing emulator. `../BTIME_PORT_PLAN.md` is the full
research write-up (memory maps, video model, derived cycle budgets, every
citation); each source file in `ArcadeMachine_BTime/src/` carries the
citations for its own facts.

## Status

**Running on real hardware.** Flashed to a Fruit Jam over USB; the game
boots, plays, and both AY-3-8910s produce the startup jingle, the coin
sound and the background music.

Frame budget, measured on device over ~5,000 frames:

```
[btime] frame 5280, frame 15715us (work 13993us, blocked 1722us),
        work_max 16405us | vblank 158608 swaps 13365 ill 0
        | audio 0us q512 under 0 peak 0
```

`work` sits at **11.6–16.2 ms of the 16.66 ms budget, peaking at 17.5 ms**
over a 23,000-frame run, with zero audio ring underruns and zero illegal
opcodes. So it fits in the ordinary case and still tops the budget in the
heaviest scenes; `DEVNOTES.md` §62 lists the remaining levers. That is comparable to Galaga (the
heaviest game here) and leaves less headroom than Donkey Kong. Getting there
took a real optimisation pass — the first flash needed 23.6 ms and showed a
red screen — and `DEVNOTES.md` §59–64 is the measured record of what worked,
what didn't, and the two changes that measured exactly zero.

Still to check on hardware: the rotation default, the aspect choice below,
and whether the audio level and tone are right (the gain was set from a
measured peak, not derived, and the cabinet's speaker filter is deliberately
omitted).

## Known issues

**A close-range pepper hit loses its "boing".** Throw pepper at a distant
enemy and you hear the toss then the hit; throw at one right next to you and
you hear only the toss. A real cabinet plays both at any distance.

The mechanism is measured: distance sets the gap between the two sound
commands, and a hit arriving within the toss effect's own ~350 ms is
declined by the sound ROM itself (3 commands sent, 3 collected — nothing is
lost). The likeliest cause is that this port's toss lasts longer than the
real one, which would need a timed recording of the toss alone from a real
machine to confirm. See `DEVNOTES.md` #74 for the full characterisation and
the ranked candidates.

Everything else reported against a real cabinet now matches: picture,
speed, and the walking effect's character and level.

## Sound

Two AY-3-8910 PSGs driven by a dedicated 500 kHz 6502, into a small
discrete network. The chip is emulated following MAME's `ay8910_device` —
tone, noise and envelope generators clocked at clock/8 = 187.5 kHz, with
MAME's own resistor-ladder amplitude tables evaluated offline for this
board's two load resistances.

The network is implemented rather than approximated, which is unusual for
this project: five channels are summed flat and scaled by 0.2, and channel
**2A alone** goes through a band-pass filter that works out to a ~187 Hz
peak with Q ≈ 1.9 and 7× gain — it is the *bass* channel, which is
consistent with MAME's note that on two 1982 recordings "the filtered sound
is way louder than the music". One part is deliberately left out: the final
3 Ω / 100 µF high-pass models the *cabinet's* 4 Ω speaker at 530 Hz, and
stacking an arcade cabinet's speaker model on top of the Fruit Jam's own
speaker models the wrong thing twice. If the result is boomier than a real
machine, that is the first thing to try adding back.

Synthesis runs on Core 0 in slices inside the scanline loop; the audio ISR
only copies out of a ring buffer. That split is the shape `DEVNOTES.md` #48
arrived at — a ~2,200-tick synthesis burst inside an interrupt would starve
the PicoDVI scanline queue, which has only ~555 µs of slack.

The one value that is **not** derived is the output level: MAME's netlist
gain is in volt-ish units that do not survive the change of amplitude
representation, so it was set by measuring the peak in the host harness
(64.5% of full scale, no clipped samples, over a 40-second capture through
the level-start music and gameplay). Adjust `OUTPUT_GAIN` in
`btime_audio.cpp` if it is wrong on real hardware.

## What's new about this machine

Four things, none of which any other game in this project has:

1. **There is no vblank interrupt.** Not "unused" — the board does not have
   one. MAME's driver header says it outright: *"These games don't have
   VBLANK interrupts, but instead an IRQ or NMI … is generated when a coin
   is inserted."* The program finds the beam by **polling bit 7 of
   `0x4003`**, a bit the schematics wire into a DIP-switch port, about 3,600
   times a frame. A port returning a constant there hangs the game.
2. **The main CPU's opcodes are encrypted, statefully.** A DECO CPU-7
   descrambles an instruction fetch if and only if a memory write has
   happened since the previous fetch *and* `(pc & 0x104) == 0x104`. Unlike
   Ms. Pac-Man's decode, the ROM therefore **cannot** be transformed once at
   load time; it lives in the fetch path forever, reached through
   `ArcadeCPU_M6502`'s optional `read_opcode` hook.
3. **The palette is RAM, not a PROM.** This board has no colour PROM at all
   — 16 bytes at `0x0C00` that the game writes, rebuilt into RGB565 on
   every write.
4. **Two 6502s**, sharing nothing but one 8-bit latch, with three separate
   interrupt mechanisms: a level-triggered IRQ (latch written), an
   edge-triggered NMI (a scanline timer at ~976 Hz), and a software enable
   for that NMI which starts *off*.

The frame loop drives all of the timing off one scanline counter — the
vblank flag, the sound NMI and the audio slices — which is honest, because
on the real board all of it divides down from the same 12 MHz crystal:
26,112 main-CPU and 8,704 sound-CPU cycles per frame, 96 and 32 per
scanline, all exact integers.

## Required assets

An SD card, FAT32-formatted with an **MBR** partition scheme (not
GPT/exFAT — macOS Disk Utility defaults to GPT on "Erase"), containing your
own legally-obtained Burger Time ROM set (MAME's `btime`, the Data East
parent set):

```
/rom/
    aa04.9b   aa06.13b  aa05.10b  aa07.15b   <- main 6502 program
    ab14.12h                                 <- sound 6502 program
    aa12.7k   ab13.9k   ab10.10k             <- charset #1 AND sprites
    ab11.12k  aa8.13k   ab9.15k                 (same bytes, two layouts)
    ab00.1b   ab01.3b   ab02.4b              <- charset #2 (background tiles)
    ab03.6b                                  <- background TILEMAP, not gfx
```

All 15 files, 52 KB total. Two things about this set are worth knowing
because they are easy to get wrong:

- **The program ROMs do not load in filename order.** `ROM_START( btime )`
  places `aa04` at `0xC000`, `aa06` at `0xD000`, `aa05` at `0xE000` and
  `aa07` at `0xF000` — 04, 06, 05, 07. This port uses an explicit manifest
  for exactly that reason.
- **`ab03.6b` is not a graphics ROM.** It is the background tilemap (which
  16×16 tile goes in which cell), and it sits in the middle of a run of
  files that otherwise are graphics.

The five program/sound ROMs are required; a missing one gives a boot-error
screen and the serial log names the file. Missing graphics ROMs are
deliberately *not* fatal — you get blank characters, sprites or background —
matching the precedent in the other machine libraries.

## Controls

| Button | Action |
|---|---|
| COIN | Insert coin |
| START1 / START2 | 1-player / 2-player start |
| UP / DOWN / LEFT / RIGHT | 4-way joystick |
| SHOOT | Throw pepper (Burger Time's one action button) |
| ROTATE | Cycle screen rotation (0°/90° CCW "tate"/180°/270° CW) |
| MIRROR | Toggle horizontal mirror (for Pepper's-Ghost half-silvered-mirror cabinets) |
| STRETCH (Button 1) | Toggle aspect-ratio correction. Which setting looks right depends on your MONITOR, not the game: a 16:9 panel already stretches a rotated picture on its own, while a panel forced to 4:3 — or a real 4:3 panel — does not. Try both and keep the one that looks correct. |

Default rotation is **1** (90° CCW, "tate"). MAME's `GAME()` line for
`btime` says `ROT270`, and across every game in this project that is
confirmed on hardware that flag has predicted the right value six times out
of six (`ROT270` → 1, `ROT90` → 3). The harness agrees — at rotation 1 the
title lands on the right-hand side of the framebuffer, which is this
project's invariant, and rotation 3 puts it on the left. Still worth
confirming with your own eyes on the physical display.

## One open decision: the aspect ratio

Burger Time's raster is **square** (240×240 visible), and every other game
here is roughly 4:3, so the project's usual 1:1 tate mapping is off by more
here than anywhere else. In tate the monitor is physically rotated, so the
raster's horizontal axis lands on the screen's long (4-unit) side; using ×1
along it leaves the picture short:

| Game | uses | of 4 units | compressed |
|---|---|---|---|
| Pac-Man | 288/320 | 3.6 | 3.6% |
| Space Invaders | 256/320 | 3.2 | 14% |
| **Burger Time** | **240/320** | **3.0** | **25%** |

So the default here shows the picture at 3/4 of its correct height, with a
40-pixel pillarbox each side. The fix is one line — spread the axis over all
320 columns — and it also makes this the only game here that fills the
screen edge to edge. It is available now via
`btime_video_set_aspect_stretch(true)` and `--stretch` in the harness, but
the **default is still 1:1** until the two have been compared on a real
display: the pillarbox makes the geometry unambiguous to eyeball during
bring-up, and consistency with the sibling games is a legitimate reason to
prefer it. See `BTIME_PORT_PLAN.md` §5.7.

## Testing without hardware

```sh
../tools/btime_host/build.sh
../tools/btime_host/btime_host --rom ../../btime_assets/rom --frames 900 \
    --coin-at 60 --start-at 200 --counters --sprites --ppm /tmp/f.ppm
```

`--counters` is the flag that matters. This machine's two worst failure
modes are both invisible on screen — if `vblank-bit reads` is zero the
program is not running at all, and if `CPU-7 opcode descrambles` is zero the
fetch hook is not working — and both look like a rendering bug. The same
counters go out with the on-device serial heartbeat for the same reason.

**One caution about the harness, learned the hard way (`DEVNOTES.md` §60):**
it runs natively and therefore has **no XIP**, so it cannot see the cost of
running code out of flash. It reported audio as 4.4% of the frame where the
device measured 27%. Use it for *what the machine is doing*; use the device's
own heartbeat for *where the time goes*.

`--wav FILE` captures exactly what the board would play, by pumping the
machine's own fill callback — the same one the audio ISR calls on device.
It also reports ring-buffer underruns and the peak sample, which is how the
output level above was set.

`--sprites` prints the eight sprites' enable/code/x/y. A machine whose
sprites are wrong shows a perfectly good playfield with nothing moving on
it, which reads as "the renderer works, something else is broken".

Beware one trap that cost real time here: **the attract demo renders a full
playfield with four sprites animating**, so a screenshot of it is
indistinguishable from a working game. Run the same sequence with no coin as
a control — if the counters and sprite positions are identical, the coin is
not getting in. See `DEVNOTES.md` #51/#52.

To read the heartbeat, configure the tty first or the port returns nothing
at all (`DEVNOTES.md` §64):

```sh
stty -f /dev/cu.usbmodem312401 115200 raw -echo
perl -e 'alarm 20; open(F,"<","/dev/cu.usbmodem312401") or die; $|=1;
         while(sysread(F,$b,256)){print $b}'
```

Set `BTIME_COST_PROFILING` to 1 in `btime_machine.cpp` and reflash to get the
per-frame cpu/render/audio breakdown in that heartbeat; it is off by default
because it costs ~1,300 `micros()` calls a frame.

There is also `../tools/m6502_test/`, which runs the standard 6502 test
suites against `ArcadeCPU_M6502` and reports cycle counts against upstream's
published figures. Worth one run after touching the CPU core.
