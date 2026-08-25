# arcade_arduino

Space Invaders for the [Adafruit Fruit Jam](https://www.adafruit.com/product/6200)
(RP2350B), running under the Arduino framework instead of the raw Pico SDK.

This is an Arduino port of [adafruit/invaders_pico](https://github.com/adafruit/invaders_pico),
restructured as **SAMP** — Single Arcade Machine Port — a small framework for
building one-game, one-board arcade firmware, organized so the pieces that
*aren't* specific to Space Invaders or to the Fruit Jam are reusable for a
future different game and/or a future different board:

- **`ArcadeCPU_i8080`** — the Intel 8080 CPU interpreter. No hardware or
  game knowledge at all.
- **`ArcadeHAL`** — plain C function contracts (video/audio/input/storage).
  No implementation lives here.
- **`ArcadeMachine_Invaders`** — Space Invaders' own port wiring, VRAM
  renderer, sample-based sound, and ROM/asset manifest. Board-agnostic —
  talks only to `ArcadeHAL`.
- **`ArcadeBoard_FruitJam`** — the Fruit Jam's implementation of
  `ArcadeHAL`: PicoDVI video, TLV320DAC3100 + I2S audio, GPIO input, SD card
  storage via FatFs.
- **`invaders_fruitjam/`** — the actual buildable sketch: the one place that
  knows both "this game" and "this board," wiring the two together.

A future Pac-Man/Z80 port, or a future different board, would each add a
sibling library alongside these — see `../CLAUDE.md` in the parent
`i8080/` checkout (if you have it) for the full architecture rationale, or
just read `ArcadeHAL/src/*.h` for the contracts themselves.

## Building

Requires an SD card, FAT32-formatted with an **MBR** partition scheme (not
GPT/exFAT — macOS Disk Utility defaults to GPT), containing your own
legally-obtained Space Invaders ROM and WAV samples:

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

### Arduino IDE

1. Install the **Raspberry Pi Pico/RP2040/RP2350** board core (Earle
   Philhower's, via Boards Manager — add
   `https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json`
   as an additional board URL first).
2. Install these libraries (Library Manager):
   `PicoDVI - Adafruit Fork`, `Adafruit TLV320 I2S`, `SdFat - Adafruit Fork`,
   `Bounce2`.
3. Set **Preferences → Sketchbook location** to this repo's root.
4. Select board **Adafruit Fruit Jam RP2350**, and
   **Tools → Optimize → Optimize More (-O2)** — the default `-Os` is not
   fast enough for this to run at full speed; see `DEVNOTES.md` #11.
5. Open `invaders_fruitjam/invaders_fruitjam.ino` and upload.

Before the full game, it's worth flashing the standalone smoke tests in
order to confirm each subsystem independently: `input_test_fruitjam` →
`dvi_test_fruitjam` → `audio_test_fruitjam` → `sd_test_fruitjam`.

### arduino-cli

```bash
arduino-cli core install rp2040:rp2040 \
  --additional-urls https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json
arduino-cli lib install "PicoDVI - Adafruit Fork" "Adafruit TLV320 I2S" \
  "SdFat - Adafruit Fork" "Bounce2"
arduino-cli compile --fqbn "rp2040:rp2040:adafruit_fruitjam:opt=Optimize2" invaders_fruitjam
```

(Each sketch also ships a `sketch.yaml` pinning `opt=Optimize2` as its
default, so `--fqbn` can be omitted once `arduino-cli`'s config points its
`directories.user` at this repo.)

## More detail

See `DEVNOTES.md` for the full account of every real bug found while
bringing this up on actual hardware — several of the fixes there
(`dvi_vertical_repeat`, the hard-coded 8-scanline-buffer ceiling, the
`-Os`-isn't-fast-enough finding) are non-obvious and worth reading before
touching `ArcadeBoard_FruitJam`.

## Credits

- Original emulator: [shotto42/invaders](https://github.com/shotto42/invaders)
- 8080 CPU core: [intarga/i8080e](https://github.com/intarga/i8080e) (MIT)
- Pico SDK port this was ported from: [adafruit/invaders_pico](https://github.com/adafruit/invaders_pico)
- DVI output: [PicoDVI](https://github.com/Wren6991/PicoDVI) by Luke Wren, via [Adafruit's fork](https://github.com/adafruit/PicoDVI)
- I2S PIO program: [pico-infoNES](https://github.com/xrp-works/pico-infoNES)
- SD card SPI driver: [wili8jam](https://github.com/wili8jam)
- FatFs: [ChaN](http://elm-chan.org/fsw/ff/)
