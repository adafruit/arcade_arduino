# pacman_fruitjam

Pac-Man on the Adafruit Fruit Jam — see the [top-level README](../README.md)
for the overall SAMP framework and general build steps. This page covers
what's specific to this game.

This is the project's **first Z80-based port** (`ArcadeCPU_Z80`, vendored
from [superzazu/z80](https://github.com/superzazu/z80), MIT). Every
hardware fact `ArcadeMachine_Pacman` relies on (memory map, I/O map,
interrupt scheme, tile/sprite decode, palette decode, Namco WSG sound
registers) was verified directly against
[MAME](https://github.com/mamedev/mame)'s own `pacman` driver source
(`src/mame/pacman/pacman.cpp`, `pacman_v.cpp`, `devices/sound/namco.cpp`) —
see that library's `.cpp` file header comments for exact citations, same
rigor `ArcadeMachine_LunarRescue`'s README applies to its own ROM/PROM map.
`picopacman-main` (an early candidate reference) turned out to contain no
CPU or hardware emulation at all — a from-scratch hand-coded clone, not an
emulator — and was not used.

## Required assets

An SD card, FAT32-formatted with an **MBR** partition scheme (not
GPT/exFAT — macOS Disk Utility defaults to GPT on "Erase"), containing your
own legally-obtained Pac-Man ROM set:

```
/rom/
    pacman.6e
    pacman.6f
    pacman.6h
    pacman.6j
    pacman.5e
    pacman.5f
    82s123.7f
    82s126.1m
    82s126.3m
    82s126.4a
```

This is the standard MAME `pacman` ROM set (10 files) — unlike Invaders'
sort-and-pack loading convention, each file is loaded to its own named
destination (see `ArcadeMachine_Pacman/src/pacman_assets.cpp`), so filename
case/order doesn't matter. `82s126.3m` is present in every real Pac-Man ROM
dump but is a "Timing" PROM MAME's own driver comments as unused by
emulation — it's fine to include it (it's simply never read) or omit it.

## Controls

| Button | Action |
|---|---|
| COIN | Insert coin |
| START1 / START2 | 1-player / 2-player start |
| UP / DOWN / LEFT / RIGHT | Move (4-way joystick) |
| ROTATE | Cycle screen rotation (0°/90° CCW "tate"/180°/270° CW) |
| MIRROR | Toggle horizontal mirror (for Pepper's-Ghost half-silvered-mirror cabinets) |

Pac-Man has no action button — `HAL_BTN_SHOOT` is unused by this game.
UP/DOWN are new physical buttons added to `ArcadeBoard_FruitJam` for this
port (header pins **A3**/**A4**, GPIO 43/44 — see `board_config_fruitjam.h`);
every other button is the same physical control the other two games use.
The button-to-action wiring lives in `pacman_fruitjam.ino` itself, not in
`ArcadeMachine_Pacman`, per the framework's usual rule.

Pac-Man's native hardware framebuffer (288x224, before the cabinet's
physical 90-degree mount) is displayed **portrait**, defaulting to rotation
**3** (90° CW). That is deliberately *not* the value Space Invaders and
Lunar Rescue default to (1): the Namco and 8080bw cabinets mounted their
monitors in opposite orientations, so the two families need opposite
software rotations to come up upright on one physical screen. All four
games in this project are therefore upright together on the same monitor
without touching the ROTATE button — see `pacman_machine.cpp`'s comment at
the default for the reasoning.

## Notes

Sound is fully synthesized (Namco WSG 3-voice wavetable), not sample-based
— Pac-Man has no WAV assets, unlike Invaders or Lunar Rescue. See
`ArcadeMachine_Pacman/src/pacman_audio.cpp` for the register map and
frequency-accumulator model, both verified against MAME's
`namco_wsg_device`.

**Confirmed working end-to-end on real hardware in tate mode**: video
(tile+sprite decode, palette), audio (Namco WSG synthesis), and full
4-way-joystick control, with no red-line/queue-starvation artifacts and no
visible tearing, plus a clean unattended overnight run of the attract
loop with no hang. Getting there took fixing several real bugs found
during bring-up — see `DEVNOTES.md` problems #18-22 for the full account:
a video-queue-starvation red screen, why `-O3` alone only shrank rather
than eliminated it, the actual fix (interleaving Z80 execution with
scanline submission), a rotation bug that mirrored the image instead of
turning it, and a 32-bit cycle-counter wraparound that permanently hung
the game after ~23 minutes of continuous play. (Problem #21's mirror fix
hasn't specifically been re-observed since — worth a quick visual check
next time tate/Yoko switching comes up, though nothing else has changed
in that code since.)

Landscape/Yoko rotation has two separate, still-open issues, both
documented in DEVNOTES and neither specific to a quick fix: the same
queue-starvation stall problem #18 fixed for tate (landscape/180 can't
use the same fix — see problem #18's own explanation of why, and problem
#20's note that a bigger redesign like double-buffering would be needed),
and — a distinct problem — the picture's aspect ratio looking visibly
wrong plus an uneven, aliased look on any regular repeating pattern
(problem #23; a startup grid test screen makes this obvious). Tate is
this project's supported orientation for this game, matching the real
Pac-Man cabinet, which is always portrait. Not yet tested: two-player
mode, extended sessions across multiple levels.
