# lrescue_fruitjam

Lunar Rescue on the Adafruit Fruit Jam — see the [top-level README](../README.md)
for the overall SAMP framework and general build steps. This page covers
what's specific to this game.

Same "8080bw" board family as Space Invaders (1.9968MHz CPU clock, same
RST1/RST2 interrupt pair — confirmed via MAME's shared `mw8080bw_root()`
machine_config), but a different ROM layout and, uniquely so far in this
project, one genuinely *synthesized* (bit-banged, not sampled) sound
channel. That channel took a real investigation to get right — see
`../DEVNOTES.md` problems #12-17 if you're touching
`ArcadeMachine_LunarRescue/src/lrescue_audio.*` or porting another game
with a similar channel.

## Build note: requires `-O3`

Unlike every other sketch in this project, `lrescue_fruitjam/sketch.yaml`
pins `opt=Optimize3` (`-O3`), not `Optimize2`. This game's per-frame
real-time margin is measurably tighter than Space Invaders' (heavier VRAM
color lookups, a larger concurrent sample set) — see `../DEVNOTES.md`
problem #16. Don't change this without re-testing on real hardware.

## Required assets

Same MBR/FAT32 SD card requirement as the top-level README. Lunar Rescue's
layout:

```
/rom/
    lrescue.1   (0x0000)
    lrescue.2   (0x0800)
    lrescue.3   (0x1000)
    lrescue.4   (0x1800)
    lrescue.5   (0x4000)
    lrescue.6   (0x4800)
/samples/
    alienexplosion.wav
    rescueshipexplosion.wav
    beamgun.wav
    thrust.wav
    bonus2.wav
    bonus3.wav
    shootingstar.wav
    stepl.wav
    steph.wav
/prom/
    7643-1.cpu
```

Two things worth knowing if you're assembling this set yourself:

- **`lrescue.5`/`lrescue.6` map to 0x4000**, not immediately after
  `lrescue.4` — this game's real board leaves 0x2000-0x3FFF as RAM, unlike
  a fully-consecutive ROM layout. `lrescue_assets.cpp` uses an explicit
  filename→address manifest for this reason (Invaders' "sort filenames,
  pack consecutively" convention doesn't fit this game — see
  `../DEVNOTES.md` #12). At minimum the four low chips (containing the
  reset vector) must be present to run at all.
- **`/prom/7643-1.cpu` is the color-map PROM**, not program code — it must
  go in `/prom/`, not `/rom/`, or it'll get swept into CPU memory
  alongside the real ROM chips. A missing color PROM is non-fatal (the
  game still runs) but loses per-block coloring.
- Sample filenames are MAME's own descriptive names (`alienexplosion.wav`,
  not a numbered scheme) — a stock MAME `lrescue` sample pack drops in
  unmodified. A missing individual sample is non-fatal and silent; the
  boot-time error screen (solid red = no SD card, solid yellow = the ROM
  set or the *entire* sample set failed to load — see the color constants
  in `lrescue_machine.h`) only fires on those two conditions, not a single
  missing file.

## Controls

| Button | Action |
|---|---|
| COIN | Insert coin — **hold at boot to enter self-test mode** instead (see below) |
| START1 / START2 | 1-player / 2-player start |
| LEFT / RIGHT | 2-way joystick |
| SHOOT | The single fire/action button (this cabinet has no separate joystick + button combo beyond this) |
| ROTATE | Cycle screen rotation (default: 90° CCW "tate") |
| MIRROR | Toggle horizontal mirror (for Pepper's-Ghost half-silvered-mirror cabinets) |

## Self-test mode

Holding COIN while the board finishes booting (after assets load, before
the first game frame) enters a self-test loop instead of starting the
game: it cycles through every sample slot and the synthesized speaker
channel in turn, showing a color-coded indicator on screen for each,
useful for confirming the full asset set loaded and every sound-trigger
path works without having to play through the actual game to trigger each
one. See `self_test_loop()` in `lrescue_fruitjam.ino`. Note this only
toggles the speaker channel on/off — it won't catch a regression in the
actual tune-reconstruction mechanism; see the isolation test below for
that.

## `lrescue_speaker_isolation_test` — regression check for the synthesized channel

A separate sketch (`../lrescue_speaker_isolation_test/`), kept around
specifically for changes to `lrescue_audio.cpp`'s speaker-synthesis
mechanism. It drives the real production mixer with the actual measured
bonus1 note sequence — no SD card, no video, no CPU emulator needed — so
it's a much faster way to sanity-check that mechanism than flashing the
full game and triggering bonus1 in play. If you touch the ring buffer,
the decimation logic, or anything around `lrescue_audio_now_cycles()`,
flash this first.

## Known limitation — diagnosed, fix not yet applied

Red horizontal lines still occur during peak simultaneous sound activity —
most reliably on the bonus arpeggio when an astronaut is returned to the
ship. The audio-quality bug this originally started from (a "crumbly"
bonus1 jingle) is fully fixed and unrelated.

**The cause is now understood.** `lrescue_run_frame()` runs the whole
frame's i8080 emulation (~1.8 ms) in one uninterrupted loop before it
submits a single scanline — the same bug `../DEVNOTES.md` problem #20 fixed
for Pac-Man, never back-applied to the i8080 games. Core 1 can only coast on
the 8-buffer scanline queue plus the vertical blanking interval, about 2 ms,
which leaves roughly **200 µs of margin**; a 232 µs audio interrupt landing
inside the burst exceeds it and Core 1 emits its red "no valid scanline"
fallback.

That also explains why it tracks audio activity, and why problem #16's long
investigation couldn't pin it down: Core 0's real per-frame work is only
~3.3 ms of the 16.7 ms budget, so nothing looked overloaded. It was never a
shortage of CPU time — it was one long uninterruptible block at the wrong
instant.

An interim mitigation is in place (the board's audio buffer halved, which
halves the interrupt's blocking window — see `hal_audio_fruitjam.cpp`). It
makes the lines markedly rarer but does not eliminate them, exactly as a
200 µs margin predicts. **The real fix — interleaving CPU execution with the
scanline pump, as `pacman_machine.cpp` already does — is not applied as of
this commit.** See `../DEVNOTES.md` problem #34 for the plan and for what to
watch for when it lands.
