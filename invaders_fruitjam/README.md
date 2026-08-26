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
