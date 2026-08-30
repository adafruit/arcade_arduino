# dkong_fruitjam

Donkey Kong on the Adafruit Fruit Jam — see the [top-level README](../README.md)
for the overall SAMP framework and general build steps. This page covers
what's specific to this game.

## Status: video and input only — no sound yet

Donkey Kong has **no sound chip**. It has an MB8884 (an 8035-class MCS-48
microcontroller) running its own program, driving a DAC directly and playing
"voice" samples from a second ROM in banked 256-byte pages, **plus** a
discrete analog network (LFSR noise, RC filters, a custom mixer) for jump,
boom and spring. Emulating it means adding a third CPU axis to this project
(`ArcadeCPU_MCS48`) and an approximation of the discrete network — a port of
its own.

Everything else is done. The audio hardware is still initialised and fed
silence, so the DAC/I2S path is exercised on every boot; adding the 8035
later means filling a buffer, not bringing up a pipeline.

## What's new about this machine

**Sprites arrive by DMA.** The Z80 never writes sprite RAM. It programs an
**i8257 DMA controller** at `0x7800-0x780F` and pulses `0x7D85`; the 8257
then copies the frame's sprite list into sprite RAM, one byte at a time,
through a latch. Nothing appears on screen without it — and the failure mode
is a perfectly good background with no Mario, no barrels and no Kong, which
looks like a renderer bug and isn't. That is why the host harness has
`--dma`.

**The inputs are ACTIVE HIGH.** Every other game in this project is active
low. Here a released button reads 0, so a zeroed shadow byte is the correct
idle state — the exact inverse of the trap that bit Galaga.

**The interrupt is an NMI**, fired at vblank and gated by a software mask at
`0x7D84`, rather than the IM0/IM1 IRQ the other Z80 games use.

**The palette is a resistor network, not a lookup.** Two PROMs feed an
analog network of weighted resistors through darlington and emitter-follower
stages into a Sanyo monitor model; the 256 RGB values are solved once at
load time. See `dkong_video.cpp`.

## Required assets

An SD card, FAT32-formatted with an **MBR** partition scheme (not GPT/exFAT
— macOS Disk Utility defaults to GPT on "Erase"), containing your own
legally-obtained Donkey Kong ROM/PROM set:

```
/rom/
    c_5et_g.bin  c_5ct_g.bin  c_5bt_g.bin  c_5at_g.bin   <- Z80 program
    v_5h_b.bin   v_3pt.bin                               <- tile ROMs
    l_4m_b.bin   l_4n_b.bin   l_4r_b.bin   l_4s_b.bin    <- sprite ROMs
    c-2k.bpr     c-2j.bpr     v-5e.bpr                   <- palette + colour-code PROMs
    s_3i_b.bin   s_3j_b.bin                              <- sound ROMs (not read yet)
```

The two sound ROMs are **not loaded** by this port — there is no sound
hardware to run them. They are listed here anyway so that a card prepared
today still works unchanged when the 8035 lands.

The four program ROMs are required; a missing one gives a boot-error screen.
Missing graphics ROMs are deliberately *not* fatal (you get blank tiles or
sprites), matching the precedent in the other machine libraries.

## Controls

| Button | Action |
|---|---|
| COIN | Insert coin |
| START1 / START2 | 1-player / 2-player start |
| UP / DOWN / LEFT / RIGHT | 4-way joystick |
| SHOOT | Jump (Donkey Kong's one action button) |
| ROTATE | Cycle screen rotation (0°/90° CCW "tate"/180°/270° CW) |
| MIRROR | Toggle horizontal mirror (for Pepper's-Ghost half-silvered-mirror cabinets) |

Jump uses the board's existing `HAL_BTN_SHOOT` (GPIO 10, header D10) — the
same physical button Space Invaders fires with. **This game needed no new
board wiring at all.**

The machine library still names its parameter `jump` rather than `shoot`:
`ArcadeMachine_DKong` only knows game-semantic actions, and which physical
button produces one is precisely the decision the sketch exists to make.
That split is why a game with an action button this project had never
modelled before touched no board file.

Default rotation is **1** (90° CCW) — *not* 3, even though Pac-Man and
Ms. Pac-Man both use 3 and this is likewise a portrait cabinet. See
`../DEVNOTES.md` #41: the invariant that actually holds is about the
framebuffer, not the manufacturer.

## Testing without hardware

`../tools/dkong_host/` builds this game's machine library — the real Z80
core, real ROMs/PROMs, the real i8257 DMA — into a native executable. See
`../tools/README.md`.

```sh
./../tools/dkong_host/build.sh
../tools/dkong_host/dkong_host --frames 3000 --dma \
    --input 600:coin,800:start1,1500:right,1700:jump --press-frames 30 \
    --ppm-every 2500 --ppm-prefix dk
```

`--dma` reports 8257 transfers, bytes moved, and the peak number of sprites
selected on any one scanline (the hardware limit is 16, and this port
emulates that limit rather than ignoring it). A healthy run shows roughly
one transfer per frame of ~384 bytes; zero transfers means sprite RAM is
never written and the screen will show a background and nothing else.
