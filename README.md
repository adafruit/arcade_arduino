# arcade_arduino

Classic arcade games for the [Adafruit Fruit Jam](https://www.adafruit.com/product/6200)
(RP2350B), running under the Arduino framework instead of the raw Pico SDK.

This started as an Arduino port of [adafruit/invaders_pico](https://github.com/adafruit/invaders_pico)
(itself a Pico SDK port of Space Invaders), restructured as **SAMP** —
Single Arcade Machine Port — a small framework for building one-game,
one-board arcade firmware, organized so the pieces that *aren't* specific
to one game or one board are reusable for the next port:

- **`ArcadeCPU_i8080`** — the Intel 8080 CPU interpreter. No hardware or
  game knowledge at all.
- **`ArcadeHAL`** — plain C function contracts (video/audio/input/storage).
  No implementation lives here.
- **`ArcadeMachine_*`** — one library per game (`ArcadeMachine_Invaders`,
  `ArcadeMachine_LunarRescue`, ...): that game's own port wiring, VRAM
  renderer, sound, and ROM/asset manifest. Board-agnostic — talks only to
  `ArcadeHAL`.
- **`ArcadeBoard_FruitJam`** — the Fruit Jam's implementation of
  `ArcadeHAL`: PicoDVI video, TLV320DAC3100 + I2S audio, GPIO input, SD card
  storage via FatFs.
- **One sketch per game** (`invaders_fruitjam/`, `lrescue_fruitjam/`, ...) —
  the one place that knows both "this game" and "this board," wiring the
  two together.

`ArcadeCPU_Z80`/`ArcadeMachine_Pacman` (below) added the project's first
Z80-based game this way — a sibling library alongside the i8080 axis, not a
replacement for it. A future different board would add a sibling
`ArcadeBoard_*` library the same way — see `../CLAUDE.md` in the parent
`i8080/` checkout (if you have it) for the full architecture rationale, or
just read `ArcadeHAL/src/*.h` for the contracts themselves.

## Games

| Game | Sketch | Notes |
|---|---|---|
| Space Invaders | [`invaders_fruitjam/`](invaders_fruitjam/README.md) | The original port; sample-based sound only. |
| Lunar Rescue | [`lrescue_fruitjam/`](lrescue_fruitjam/README.md) | Same "8080bw" board family, plus one genuinely *synthesized* (bit-banged) audio channel — see its README and `DEVNOTES.md` problems #12-17 for what that took. |
| Pac-Man | [`pacman_fruitjam/`](pacman_fruitjam/README.md) | The project's first **Z80**-based port (`ArcadeCPU_Z80`) and first tile+sprite video hardware (`ArcadeMachine_Pacman`), with fully synthesized Namco WSG sound — built from scratch against the real ROM/PROM dump and verified against MAME's driver source; see its README for citations. |

Each game's own README covers its specific ROM/sample layout, controls, and
any known quirks. Both share the building steps below.

## Building

### Arduino IDE

1. Install the **Raspberry Pi Pico/RP2040/RP2350** board core (Earle
   Philhower's, via Boards Manager — add
   `https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json`
   as an additional board URL first).
2. Install these libraries (Library Manager):
   `PicoDVI - Adafruit Fork`, `Adafruit TLV320 I2S`, `SdFat - Adafruit Fork`,
   `Bounce2`.
3. Set **Preferences → Sketchbook location** to this repo's root.
4. Select board **Adafruit Fruit Jam RP2350**. Each sketch pins its own
   required **Tools → Optimize** level via a `sketch.yaml` (see
   `DEVNOTES.md` #11 and #16 for why this differs per game — the default
   `-Os` is not fast enough for any of them), so you generally don't need
   to set this by hand, but double-check it matches that sketch's
   `sketch.yaml` if the IDE doesn't pick it up automatically.
5. Prepare an SD card (FAT32, **MBR** partition scheme — not GPT/exFAT,
   which macOS Disk Utility defaults to on "Erase") with that game's own
   ROM/sample layout — see its README.
6. Open that game's `.ino` and upload.

Before a full game, it's worth flashing the standalone smoke tests in
order to confirm each subsystem independently: `input_test_fruitjam` →
`dvi_test_fruitjam` → `audio_test_fruitjam` → `sd_test_fruitjam`.

### arduino-cli

```bash
arduino-cli core install rp2040:rp2040 \
  --additional-urls https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json
arduino-cli lib install "PicoDVI - Adafruit Fork" "Adafruit TLV320 I2S" \
  "SdFat - Adafruit Fork" "Bounce2"
arduino-cli compile invaders_fruitjam   # or: lrescue_fruitjam, etc.
```

(Each sketch's `sketch.yaml` pins its own required `opt=` level as the
default `--fqbn`, so it can be omitted once `arduino-cli`'s config points
its `directories.user` at this repo.)

## More detail

See `DEVNOTES.md` for the full account of every real bug found while
bringing this up on actual hardware, across both the shared framework and
each individual game port — several of the fixes there
(`dvi_vertical_repeat`, the hard-coded 8-scanline-buffer ceiling, the
`-Os`-isn't-fast-enough finding, the cycle-vs-real-time audio-clock lesson)
are non-obvious and worth reading before touching `ArcadeBoard_FruitJam`,
`ArcadeCPU_i8080`, or adding a new synthesized-audio channel to any game.

## Credits

- Original Space Invaders emulator: [shotto42/invaders](https://github.com/shotto42/invaders)
- 8080 CPU core: [intarga/i8080e](https://github.com/intarga/i8080e) (MIT)
- Z80 CPU core: [superzazu/z80](https://github.com/superzazu/z80) (MIT)
- Pac-Man's memory map, I/O map, tile/sprite/palette decode, and Namco WSG
  sound register map were all verified against
  [MAME](https://github.com/mamedev/mame)'s `pacman` driver source, not
  ported from any existing emulator — see `ArcadeMachine_Pacman/src/`'s own
  file-header comments for exact citations.
- Pico SDK port this was ported from: [adafruit/invaders_pico](https://github.com/adafruit/invaders_pico)
- DVI output: [PicoDVI](https://github.com/Wren6991/PicoDVI) by Luke Wren, via [Adafruit's fork](https://github.com/adafruit/PicoDVI)
- I2S PIO program: [pico-infoNES](https://github.com/xrp-works/pico-infoNES)
- SD card SPI driver: [wili8jam](https://github.com/wili8jam)
- FatFs: [ChaN](http://elm-chan.org/fsw/ff/)
- Lunar Rescue's ROM/color-PROM map and sound-trigger wiring were verified
  against [MAME](https://github.com/mamedev/mame)'s `midw8080` driver
  source, not inferred by analogy — see `ArcadeMachine_LunarRescue/src/`'s
  own file-header comments for the exact formulas and where each came from.
