<!--
SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
SPDX-License-Identifier: MIT
-->

# invaders_fruitjam

Space Invaders on the Adafruit Fruit Jam — see the [top-level README](../README.md)
for the overall SAMP framework and general build steps. This page covers
what's specific to this game.

## Required assets

An SD card, FAT32-formatted with an **MBR** partition scheme (not
GPT/exFAT — macOS Disk Utility defaults to GPT on "Erase"), containing your
own legally-obtained Space Invaders ROM and WAV samples:

```
/rom/
    invaders.h
    invaders.g
    invaders.f
    invaders.e
/samples/
    0.wav
    ...
    9.wav
```

ROM chip files are loaded by listing `/rom/` and sorting filenames
**reverse-alphabetically**, placing each at the next consecutive memory
address starting at 0x0000 — the standard MAME `.h`/`.g`/`.f`/`.e` chip
naming happens to sort into the correct order for this ROM set. Sample
files use MAME's own numbered filenames (`0.wav`-`9.wav`), not descriptive
names.

## Controls

| Button | Action |
|---|---|
| COIN | Insert coin |
| START1 / START2 | 1-player / 2-player start |
| LEFT / RIGHT | Move |
| SHOOT | Fire |
| ROTATE | Cycle screen rotation (0°/90° CCW "tate"/180°/270° CW) |
| MIRROR | Toggle horizontal mirror (for Pepper's-Ghost half-silvered-mirror cabinets) |
| STRETCH (Button 1) | Toggle aspect-ratio correction. Which setting looks right depends on your MONITOR, not the game: a 16:9 panel already stretches a rotated picture on its own, while a panel forced to 4:3 — or a real 4:3 panel — does not. Try both and keep the one that looks correct. |

Physical GPIO mapping for these lives in `ArcadeBoard_FruitJam`'s
`board_config_fruitjam.h` — the button-to-action wiring above lives in
`invaders_fruitjam.ino` itself, not in `ArcadeMachine_Invaders`, since this
sketch is the one place that knows both which physical button is which and
what each one means for this specific game.

## Notes

Audio is entirely sample-based (10 WAV files, MAME's own `0.wav`-`9.wav`
naming) — Space Invaders has no synthesized sound channel, unlike Lunar
Rescue. See `../DEVNOTES.md` problem #7 for the one real audio-related bug
found on this port (a dropped `__not_in_flash_func` on the mixer callback).

`invaders_run_frame()` interleaves i8080 execution with scanline output
rather than running a whole frame's emulation and only then rendering — see
`../DEVNOTES.md` problem #36, and #34 for the red lines the un-interleaved
shape caused in Lunar Rescue. This game's lighter audio meant it never
showed the symptom, but it had the same ~200us of margin.

The sketch carries a once-per-second `[invaders] frame ...` heartbeat
printing `frame`/`work`/`blocked`/`work_max`. `work` (frame time minus time
blocked in `hal_video_acquire_scanline()`) is the only one that means
anything on its own — see the comment at the call site. Safe to delete once
the number is recorded in DEVNOTES.

## Testing without hardware

`../tools/invaders_host/` builds this game's machine library — the real i8080
core, real ROMs, real port decode, real frame interleaving — into a native
executable. See `../tools/README.md`.

```sh
./../tools/invaders_host/build.sh
../tools/invaders_host/invaders_host --frames 4000 --ppm-every 1000 \
    --input 100:coin,160:start1,400:shoot,600:left,900:right
```

It finds `invaders_assets/` (both `rom/` and `samples/`) by searching
upward, or takes `--assets DIR`. `--digest-every N` hashes the whole
emulated machine, which is how problem #36's interleave was shown not to
change emulation.
