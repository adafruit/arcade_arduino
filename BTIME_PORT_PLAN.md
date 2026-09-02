<!--
SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
SPDX-License-Identifier: MIT
-->

# Burger Time port plan (`btime`, Data East 1982)

**STATUS: the port is written and runs.** This file is kept as the
pre-implementation research it was — every hardware fact below was read out of
MAME's actual driver source (fetched, not recalled), and every citation names
the file and construct it came from, so any of it can be re-checked in one
command.

What exists now — **and it runs on real hardware**, at a measured `work` of
11.6–16.4ms against the 16.66ms budget after an optimisation pass recorded in
`DEVNOTES.md` §59–64:

- `libraries/ArcadeCPU_M6502/` — the CPU axis, with `tools/m6502_test/`
  reporting PASS on Klaus Dormann's functional test, the decimal test and
  AllSuiteA at cycle counts identical to upstream's.
- `libraries/ArcadeMachine_BTime/` — memory maps, the DECO CPU-7 descrambler,
  char/sprite/background video, input, and two emulated AY-3-8910s with the
  board's discrete network.
- `btime_fruitjam/` — the sketch, compiling at 113,508 bytes flash and
  154,848 bytes RAM (29%).

**Read `DEVNOTES.md` §50-57 for what actually happened when it ran**, which is
the part this document could not predict: two silent failure modes and the
counters that separate them, the attract demo masquerading as a working game,
a ten-cycle interrupt window that made per-scanline interrupt checks useless,
and the two audio measurements that caught a boot thump and set the output
level. Section 12 below is annotated with which of its open questions are now
answered.

**Sources used throughout** (upstream `mamedev/mame`, `master`):

| What | Where |
|---|---|
| Driver, video, netlist, ROM defs, DIPs | `src/mame/dataeast/btime.cpp` (3293 lines — video is *in* the driver, there is no `btime_v.cpp`) |
| Main CPU encryption | `src/mame/dataeast/decocpu7.cpp` / `.h` |
| PSG | `src/devices/sound/ay8910.cpp` |
| Sound-latch semantics | `src/devices/machine/gen_latch.cpp` |
| Standard 3bpp char layout | `src/emu/video/generic.cpp` (`gfx_8x8x3_planar`) |
| Plane→bit order | `src/emu/drawgfx.cpp` (`gfx_element::decode`) |

---

## 1. The ROM set on hand is complete and correct

`btime_assets/rom/` holds 15 files, 53,248 bytes. Every CRC32 matches
`ROM_START( btime )` — the **parent** set, "Burger Time (Data East set 1)" —
with nothing missing and nothing extra:

| File | Size | CRC32 | MAME region | Load offset |
|---|---|---|---|---|
| `aa04.9b` | 4096 | `368a25b5` | `maincpu` | `0xC000` |
| `aa06.13b` | 4096 | `b4ba400d` | `maincpu` | `0xD000` |
| `aa05.10b` | 4096 | `8005bffa` | `maincpu` | `0xE000` |
| `aa07.15b` | 4096 | `086440ad` | `maincpu` | `0xF000` |
| `ab14.12h` | 4096 | `f55e5211` | `audiocpu` | `0xE000` (mirrored at `0xF000`) |
| `aa12.7k` | 4096 | `c4617243` | `gfx1` | `0x0000` |
| `ab13.9k` | 4096 | `ac01042f` | `gfx1` | `0x1000` |
| `ab10.10k` | 4096 | `854a872a` | `gfx1` | `0x2000` |
| `ab11.12k` | 4096 | `d4848014` | `gfx1` | `0x3000` |
| `aa8.13k` | 4096 | `8650c788` | `gfx1` | `0x4000` |
| `ab9.15k` | 4096 | `8dec15e6` | `gfx1` | `0x5000` |
| `ab00.1b` | 2048 | `c7a14485` | `gfx2` | `0x0000` |
| `ab01.3b` | 2048 | `25b49078` | `gfx2` | `0x0800` |
| `ab02.4b` | 2048 | `b8ef56c3` | `gfx2` | `0x1000` |
| `ab03.6b` | 2048 | `d26bc1f3` | `bg_map` | `0x0000` |

Note the two easy-to-misfile ones: **`ab03.6b` is not a graphics ROM**, it is
the background *tilemap* (which 16×16 tile goes in which cell), and the
`maincpu` load order is `aa04, aa06, aa05, aa07` — **not** filename order.
This set therefore needs an explicit filename→destination manifest, exactly
like `dkong_assets.cpp` and unlike Invaders' "sort reverse-alphabetically and
place consecutively" shortcut (`DEVNOTES.md` #12).

There is **no color PROM** in this set, and that is not an omission — see §5.4.

Proposed SD layout (`/rom/`, flat, same as every other game): all 15 files
under their MAME names. Program ROMs and `gfx1` fatal on failure; `gfx2` /
`bg_map` non-fatal (blank backgrounds), matching precedent.

---

## 2. What the board is

| | |
|---|---|
| Main CPU | **DECO CPU-7** — an *encrypted* MOS 6502 — at 12MHz/8 = **1.5 MHz** |
| Sound CPU | plain **M6502** at 12MHz/24 = **500 kHz** |
| PSG | **2 × AY-3-8910** at **1.5 MHz**, into a small discrete network |
| Video | 6 MHz dot clock, htotal 384 (visible 8–247), vtotal 272 (visible 8–247) → **240×240 visible**, **57.4449 Hz**, `ROT270` |
| Palette | **16 bytes of RAM** at `0x0C00`, `BGR_233_inverted`. No PROM. |
| Interrupts | **no vblank interrupt at all.** Main CPU IRQ fires only on *coin insert*. |

From `btime_state::btime(machine_config&)` (btime.cpp:2297) and
`GAME( 1982, btime, 0, btime, btime, btime_state, init_btime, ROT270, ...)`
(btime.cpp:3267). Guru's PCB notes in the driver header measured VSync
57.4358 Hz / HSync 15.6235 kHz on real hardware, against 57.4449 / 15.625
from an ideal 12.000 MHz XTAL — the difference is the crystal, not a modelling
choice.

Because CPU and video derive from the same 12 MHz XTAL, the budgets are exact
integers with no remainder — worth stating up front because it makes the frame
loop in §7 clean:

```
main CPU  = dot clock / 4   →  26,112 cycles / frame  =  96 cycles / scanline
sound CPU = dot clock / 12  →   8,704 cycles / frame  =  32 cycles / scanline
                               (272 scanlines / frame, both exact)
```

For scale: that is **34,816 6502 cycles per frame total**, against Donkey
Kong's 50,688 Z80 cycles *plus* its 8035. 6502 instructions average fewer
cycles than Z80 ones, but this is still the lightest CPU load of any tile-and-
sprite machine in this project. Video and audio are where the budget goes.

---

## 3. Seven things that are new for this project

1. **A 6502 axis.** `ArcadeCPU_M6502` would be the fourth CPU family after
   i8080/Z80/MCS-48. See §4.
2. **The DECO CPU-7 opcode scramble** — and it is *stateful*, so the ROM
   **cannot** be pre-decrypted at load time. See §4.2. This is a smaller job
   than Ms. Pac-Man's decode but a different shape: hers was a static ROM
   transform, this one lives in the fetch path forever.
3. **An AY-3-8910.** The project has synthesized Namco WSG, a Namco 54XX, and
   an 8035-plus-DAC, but never a general-purpose PSG. Two of them here. See §6.
4. **The game polls video timing instead of being interrupted by it.** There
   is no vblank IRQ; the program reads a **vblank bit that is wired into a DIP
   switch port**. A port that returns a constant there will hang in a wait
   loop. This is `DEVNOTES.md` #24 ("`PORT_DIPUNUSED` does not mean reads 0")
   in a new costume, and it is the single most likely cause of a first-light
   black screen. See §5.6 and §7.
5. **The palette is RAM the game writes**, not a PROM decoded once at boot.
   Colors can change mid-frame in principle; recompute the 16-entry RGB565 LUT
   on write (or once per frame) rather than once at load.
6. **A square 240×240 raster.** Every previous game here has a roughly 4:3
   raster, so the project's 1:1 tate mapping was never far off. On a square
   raster it is off by a quarter, which makes this the first game where the
   aspect question needs an actual decision. See §5.7.
7. **Two CPUs of the same family sharing nothing but a latch.** Simpler than
   Galaga's three-Z80 shared-RAM arrangement: one 8-bit write-only latch, one
   IRQ, one NMI. See §6.3.

---

## 4. CPU

### 4.1 Which core

Recommendation: vendor **[superzazu/6502](https://github.com/superzazu/6502)**
(MIT, Nicolas Allemand) as `ArcadeCPU_M6502`. Rationale: it is by the same
author as the `z80` core already vendored as `ArcadeCPU_Z80`, so the API shape
(`init`/`step`/`read_byte`/`write_byte` function pointers, a free-running
`cyc` counter) and the license story are both already precedented here; and
its own README reports passing `6502_functional_test`, `6502_decimal_test`,
`AllSuiteA` and `timingtest`.

Two caveats to deal with deliberately rather than discover on hardware:

- ~~**It is a 65C02, and the CPU-7 is NMOS.**~~ **THIS TURNED OUT TO BE
  WRONG, and in the helpful direction.** The core emulates BOTH an NMOS 6502
  and a 65C02, selected by an `m65c02_mode` flag that `m6502_init()` leaves
  at **0** — so the default is already the NMOS part, it has separate cycle
  tables for each, and the NMOS `JMP ($xxFF)` page-wrap bug is implemented
  and correctly gated on that flag. No NMOS-ification was needed. The
  concern was worth having and cost one look at the source to dismiss.
  (Illegal opcodes are still counted, via the `illegal_ops` counter added to
  the core: over every run so far, including 5,000-frame games, the count is
  **zero** — consistent with MAME needing an undocumented-opcode patch for
  *Zoar* on this same board (`init_zoar()`, btime.cpp:3181) and none for
  btime.)
- **Its README reports it does *not* pass `6502_interrupt_test`.** Burger Time
  uses one IRQ (coin), one level-triggered IRQ (sound latch) and one
  edge-triggered NMI (sound timer), so read that core's interrupt path before
  trusting it, and consider building Klaus Dormann's interrupt test into
  `tools/` — the harness makes that nearly free and it is exactly the kind of
  thing that otherwise costs a hardware cycle.

`ArcadeCPU_M6502` must be plain C (like `z80.c` and `mcs48.c`) so the harness
can compile it with `cc` and the sketch with the Arduino toolchain.

### 4.2 The DECO CPU-7 scramble — the exact rule

From `deco_cpu7_device::mi_decrypt` (decocpu7.cpp:28-42), in full:

```cpp
uint8_t deco_cpu7_device::mi_decrypt::read_sync(uint16_t adr) {
    uint8_t res = m_cprogram.read_byte(adr);
    if (m_had_written) {
        m_had_written = false;
        if ((adr & 0x0104) == 0x0104)
            res = bitswap<8>(res, 6,5,3,4,2,7,1,0);
    }
    return res;
}
void deco_cpu7_device::mi_decrypt::write(uint16_t adr, uint8_t val) {
    m_program.write_byte(adr, val);
    m_had_written = true;
}
```

Read carefully, that is:

- Only **opcode fetches** are affected (`read_sync`). Operand and data reads
  are never descrambled — and, importantly, they do **not** clear the flag,
  because `mi_decrypt` overrides only `read_sync` and `write`.
- The flag means "**has any write happened since the last opcode fetch**", not
  "was the immediately preceding bus cycle a write". That distinction is what
  makes this implementable in an instruction-level core: set a flag whenever
  the instruction writes memory; test-and-clear it at the next opcode fetch.
  (It is also *observably* the right reading: after `JSR`, whose final bus
  cycle is a read of the target's high byte, the flag survives into the next
  fetch.)
- The address test is on the **fetch address**: `(pc & 0x0104) == 0x0104`.
- The transform, with MAME's `bitswap<8>(v, b7..b0)` meaning "result bit 7 =
  source bit `b7`":

  | out bit | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
  |---|---|---|---|---|---|---|---|---|
  | from in bit | 6 | 5 | 3 | 4 | 2 | 7 | 1 | 0 |

- **Interrupts inherit it.** Taking an IRQ/NMI pushes PCH, PCL and P — three
  writes — and then fetches the handler's first opcode, so that opcode *is*
  descrambled if its address satisfies the mask. Reproduce this or handlers at
  matching addresses will execute garbage.
- Reset clears the flag (`device_reset`).

Cost: one bool, one branch and a table lookup in the fetch path. The
`ArcadeCPU_M6502` API needs a hook the machine can install for this — cleanest
is a separate `read_opcode` callback alongside `read_byte`, defaulting to
`read_byte`, so the CPU library stays free of Data East knowledge (SAMP's
rule) and `btime_ports.cpp` owns the scramble.

### 4.3 Main CPU memory map

Verbatim from `btime_state::btime_map()` (btime.cpp:1034):

```
0000-07FF  RAM (2K)
0C00-0C0F  palette RAM (16 bytes, write-only path in MAME)
1000-13FF  video RAM  (1K, char codes)
1400-17FF  color RAM  (1K, low 2 bits = char code bits 8-9)
1800-1BFF  video RAM, X/Y SWAPPED (read+write)
1C00-1FFF  color RAM, X/Y SWAPPED (read+write)
4000       r: P1            w: (ignored)
4001       r: P2
4002       r: SYSTEM        w: video control (bit 0 = flip screen)
4003       r: DSW1          w: sound latch  (and asserts sound CPU IRQ)
4004       r: DSW2          w: bnj_scroll[0] (background enable/page/scroll)
B000-FFFF  ROM  (this set populates C000-FFFF only; B000-BFFF reads open bus)
```

The swapped mirrors are a real hardware feature, not a MAME convenience — two
address decoders onto the same RAM, one with X and Y exchanged
(`btime_mirrorvideoram_r/w`, btime.cpp:534-563):

```c
x = offset / 32;  y = offset % 32;  offset = 32*y + x;
```

The program uses whichever port suits the shape it is drawing. **Sprite
attributes live in the first row of the swapped window**, which is why they
appear as the first *column* of ordinary video RAM (§5.2) — the driver header
says this explicitly.

### 4.4 Sound CPU memory map

From `btime_state::audio_map()` (btime.cpp:1214):

```
0000-03FF  RAM (mirrored through 1C00)
2000-3FFF  w: AY1 data
4000-5FFF  w: AY1 address
6000-7FFF  w: AY2 data
8000-9FFF  w: AY2 address
A000-BFFF  r: sound latch   ← reading it CLEARS the IRQ
C000-DFFF  w: audio NMI enable (bit 0)
E000-EFFF  ROM (mirrored at F000-FFFF, so the reset/NMI/IRQ vectors resolve)
```

Note the enormous mirrored decode windows — one address line each. A port that
only decodes the exact base addresses will silently drop register writes.

---

## 5. Video

Native raster 256×256, of which **x 8–247 and y 8–247 are visible** (240×240)
per the `set_raw(6MHz, 384, 8, 248, 272, 8, 248)` call. MAME's draw functions
work in raw 0–255 coordinates and let the cliprect do the cropping, so the
port needs to carry that 8-pixel offset on both axes explicitly.

Draw order, from `screen_update_btime()` (btime.cpp:764):

```
if (bnj_scroll[0] & 0x10) { draw_background();  draw_chars(transparent); }
else                      { draw_chars(opaque); }
draw_sprites();          // always, pen 0 transparent
```

### 5.1 Characters — 32×32 grid of 8×8, 3bpp

`draw_chars()` (btime.cpp:655):

```c
x    = 31 - (offs / 32);          // note the reversal
y    = offs % 32;
code = videoram[offs] + 256 * (colorram[offs] & 3);   // 0..1023
// drawn at (8*x, 8*y), gfx(0), color 0
```

Inverted for a renderer that walks the screen: the VRAM offset for character
cell `(cx, cy)` is `32*(31-cx) + cy`. For a fixed row `cy`, stepping `cx`
walks the offset down by 32 — a strided read, cheap either way.

`code` spans 0–1023, which is exactly the 1024 characters in `gfx1`
(`gfx_8x8x3_planar` over `RGN_FRAC(1,3)` = 0x2000 bytes/plane ÷ 8 bytes/char).
Colour RAM's low 2 bits are **code** bits 8–9, *not* a palette selector — the
whole charset shares palette pens 0–7.

Plane addressing (from `gfx_8x8x3_planar` + `gfx_element::decode`, where
`planeoffset[0]` is the **MSB**: `planebit = 1 << (planes-1)` for plane 0):

```
pen bit 2 (MSB) : gfx1[0x4000 + code*8 + row]
pen bit 1       : gfx1[0x2000 + code*8 + row]
pen bit 0       : gfx1[0x0000 + code*8 + row]
x = 0 is bit 7 of each byte (xoffs STEP8(0,1), MSB first)
```

Cache: `char_px[1024][8][8]` as `[code][row][col]` = **64 KB**, giving 8
contiguous bytes per character row. (Deliberately *not* Pac-Man's
`[tile][x][y]` order — this renderer's inner loop walks a row, so row-major is
the one that memcpy-es.) Build once in `btime_video_build_caches()`.

### 5.2 Sprites — 8 of them, 16×16, interleaved into video RAM

`draw_sprites(..., sprite_ram = m_videoram, interleave = 0x20)`
(btime.cpp:683). Sprite `i` has base `offs = i * 4 * 0x20 = i * 0x80`, and its
four bytes are `0x20` apart:

| Byte | Meaning |
|---|---|
| `videoram[offs + 0x00]` | bit 0 = enable, bit 1 = flip Y, bit 2 = flip X |
| `videoram[offs + 0x20]` | tile code (0–255) |
| `videoram[offs + 0x40]` | Y, as `y = 240 - value`, then `y -= 1` |
| `videoram[offs + 0x60]` | X, as `x = 240 - value` |

Both coordinates are **subtractive**, and the Y adjust of 1 is btime-specific
(`sprite_y_adjust = 1` in its `screen_update`; Eggs on the same code path
passes 0). Every sprite is **drawn twice**, at `y` and at `y + 256`, to get
hardware wraparound. Pen 0 is transparent.

Layout is `tile16layout` (btime.cpp:2077) over the *same* `gfx1` region as the
characters — 0x2000 bytes/plane ÷ 32 = 256 sprites. Working the bit offsets
through (`xoffs = { STEP8(16*8,1), STEP8(0,1) }`, `yoffs = STEP16(0,8)`,
increment 32 bytes):

```
row r, plane p at base { 0x4000, 0x2000, 0x0000 } (MSB first):
  x  0..7  ← byte[base + code*32 + 16 + r], bit 7 → bit 0
  x  8..15 ← byte[base + code*32 +      r], bit 7 → bit 0
```

Only 8 sprites are live per frame, so **decode them once per frame** into an
`[8][16][16]` scratch (2 KB) rather than caching all 256 (which would be
another 64 KB) or re-extracting bits per scanline. That is lever 2 of the
playbook and it applies cleanly here.

### 5.3 Background — 16×16 tiles from ROM, enabled by a port bit

Off unless `bnj_scroll[0] & 0x10` (i.e. bit 4 of the byte written to `0x4004`).
`draw_background()` (btime.cpp:728) plus its caller:

```c
// caller builds the 4-entry page list:
start = flip_screen ? 0 : 1;
for (i = 0; i < 4; i++) { tilemap[i] = start | (bnj_scroll[0] & 0x04);
                          start = (start + 1) & 3; }
// draw:
scroll = -(bnj_scroll[1] | ((bnj_scroll[0] & 0x03) << 8));   // bnj_scroll[1]
                                                             // is always 0 here
for (i = 0; i < 5; i++, scroll += 256) {
    tileoffset = tilemap[i & 3] * 0x100;
    if (scroll > 256) break;  if (scroll < -256) continue;
    for (offs = 0; offs < 0x100; offs++) {
        x = 240 - (16 * (offs / 16) + scroll) - 1;   // note the -1
        y = 16 * (offs % 16);
        gfx(2)->opaque(bg_map[tileoffset + offs], color 0, x, y);
    }
}
```

So: `bg_map` (`ab03.6b`, 2 KB) is **8 pages of 256 entries**, each page a
16×16 arrangement of 16×16-pixel tiles = one 256×256 screen. `bnj_scroll[0]`
bits 0–1 pick the coarse 256-pixel offset, bit 2 picks the page bank, bit 4 is
the enable. Tiles come from `gfx2` — `tile16layout` again, `RGN_FRAC(1,3)` of
0x1800 = 0x800/plane ÷ 32 = **64 tiles**, plane bases `{0x1000, 0x0800,
0x0000}` — and they use **palette pens 8–15** (`GFXDECODE_ENTRY("gfx2", 0,
tile16layout, 8, 1)`, btime.cpp:2135). Cache: `bg_px[64][16][16]` = 16 KB.

Total decode caches: 64 KB chars + 16 KB background + 2 KB per-frame sprites =
**82 KB**, plus 30 KB of staging for the graphics ROMs that can be freed after
the build (or kept — there is room either way on a 520 KB part).

### 5.4 Palette — 16 bytes of RAM, no PROM

The driver comment above `btime_palette()` (btime.cpp:412) is explicit:
"Burger Time doesn't have a color PROM. It uses RAM to dynamically create the
palette." `btime_palette()` itself does nothing for this set (it early-returns
when there is no `proms` region); the format comes from the `machine_config`:

```cpp
PALETTE(config, m_palette, FUNC(btime_state::btime_palette))
    .set_format(palette_device::BGR_233_inverted, 16);
```

and the resistor network the comment documents:

```
bit 7 -- 15k -- BLUE  (inverted)      bit 2 -- 15k -- RED (inverted)
bit 6 -- 33k -- BLUE  (inverted)      bit 1 -- 33k -- RED (inverted)
bit 5 -- 15k -- GREEN (inverted)      bit 0 -- 47k -- RED (inverted)
bit 4 -- 33k -- GREEN (inverted)
bit 3 -- 47k -- GREEN (inverted)
```

So: invert the byte, then red = bits 0–2, green = bits 3–5, blue = bits 6–7,
expanded 3→8 and 2→8 bits (MAME's `pal3bit`/`pal2bit`, i.e. `i*255/7` and
`i*255/3`). Note that this is *not* the same as weighting by the actual
resistors, which is what `resnet.cpp` did for Donkey Kong; MAME deliberately
uses the even expansion here, and matching MAME is the point (it is what the
game is verified against). Recompute the 16-entry RGB565 LUT on any write to
`0x0C00-0x0C0F`.

Pens 0–7 are chars and sprites; 8–15 are background.

### 5.5 Flip screen

`btime_video_control_w()` (btime.cpp:606) — bit 0 of the byte written to
`0x4002` sets flip screen, unconditionally for btime (the DIP-gated variant is
`bnj_video_control_w`, used by other games in the driver). Cocktail cabinets
use it, so it will be exercised if DSW1 bit 6 is ever set to Cocktail. Both
the char and sprite paths flip coordinates *and* flip the graphics
(`draw_chars` passes `flip_screen()` as both flipx and flipy). Support it —
`ArcadeMachine_Pacman`/`DKong` both do — but leave DSW1 at Upright.

### 5.6 The vblank bit (do not skip this)

`DSW1` is read at `0x4003`, and in the `btime` input port definition its
**bit 7 is not a DIP switch**:

```cpp
PORT_BIT( 0x80, IP_ACTIVE_HIGH, IPT_CUSTOM )
    PORT_READ_LINE_DEVICE_MEMBER("screen", FUNC(screen_device::vblank))
    // Schematics show this is connected to DIP SW2.8
```

With no vblank interrupt anywhere in the machine, this bit is how the program
knows where the beam is. It must read **1 during vertical blanking** — i.e.
outside visible lines 8–247 of the 272-line frame — and change within the
frame. Return a constant and the game hangs; return it inverted and it will
draw during the visible area and tear or corrupt.

The driver header's other warning applies to the same circuit: *"Most games
have SW2.8 on… it must be on regardless or those games won't boot."*

### 5.7 Mapping 240×240 onto the Fruit Jam framebuffer

The geometry the existing renderers were calibrated against
(`invaders_video.cpp`'s header, the source of the constants every later game
copied): the visible window is **DVI x 0–319** and DVI y 0–479, and on this
monitor at this resolution **a DVI x step is physically twice as wide as a DVI
y step**. Since `dvi_vertical_repeat = 2` makes one *submitted* scanline two
physical y steps tall, the effective drawing surface is a **320 × 240 grid of
square pixels** filling the 4:3 screen.

Tate mode then maps the native raster's vertical axis (`GAME_HEIGHT`) onto the
240 submitted rows at ×2, and its horizontal axis (`GAME_WIDTH`) onto the
scanline buffer at ×1 — confirmed against both `invaders_machine.h`
(`GAME_WIDTH 256`, `GAME_HEIGHT 224`) and `pacman_machine.h` (`288`, `224`).
Burger Time's raster is **square**, so both constants come out the same
whichever way round they are named — a small mercy:

```
TATE_BY = (480 - 240*2) / 2 = 0        // native y fills all 240 rows, 1:1
TATE_BX = (320 - 240)   / 2 = 40       // native x centred, 40px pillarbox
```

That 1:1 mapping is what house style would give, and it is worth being precise
about what it costs rather than calling it wrong. In tate the monitor is
physically rotated, so the native-x axis lands on the screen's long (4-unit)
side and native-y on its short (3-unit) side. A real `ROT270` cabinet fills
that portrait screen: 4 units along native x, 3 along native y. This project's
tate always uses ×1 in the native-x axis, so every game here is somewhat
compressed along it:

| Game | native W → units (of 4) | native H → units (of 3) | compression along native x |
|---|---|---|---|
| Pac-Man | 288/320 → 3.6 | 224/240 → 2.8 | 3.6% |
| Space Invaders | 256/320 → 3.2 | 224/240 → 2.8 | 14% |
| **Burger Time** | **240/320 → 3.0** | **240/240 → 3.0** | **25%** |

So the existing games are not aspect-correct either; Pac-Man is nearly right by
luck. Burger Time, being square in a project whose other rasters are already
roughly 4:3, would be the most compressed by a wide margin — a picture 3/4 of
its correct height. Fixing it is one line, because the fix is to use the 320
columns that are already there:

```
nx = col * 3 / 4        // col = 0..319, nx = 0..239   (or a 320-entry LUT)
```

That is the same non-integer stretch `invaders_video.cpp` already applies in
*landscape* (`dy = dvi_y * 256 / 480`), just on the other axis, and it happens
to make Burger Time the only game here that fills the screen edge to edge with
no border at all.

Suggested plan: build the 1:1 version first because it is trivial to eyeball
for correctness, then add the stretch and compare the two on the physical
display — only that settles it, and it is also the moment to decide whether
consistency with the siblings or fidelity to the cabinet wins.

**Rotation default: predict 1, then verify.** `DEVNOTES.md` #33/#41 rightly
say this value cannot be copied from a neighbouring game — but it turns out it
*can* be predicted from MAME, which is a cheaper starting point than guessing.
Checking every game in this project against its MAME `GAME()` flag:

| Game | MAME flag | This project's default | Confirmed on hardware |
|---|---|---|---|
| Space Invaders | `ROT270` | 1 | yes |
| Lunar Rescue | `ROT270` | 1 | yes |
| Donkey Kong | `ROT270` | 1 | yes |
| Pac-Man | `ROT90` | 3 | yes |
| Ms. Pac-Man | `ROT90` | 3 | yes |
| Galaga | `ROT90` | 3 | yes |

Six for six: `ROT270` → 1, `ROT90` → 3. Burger Time is `ROT270`, so **start at
rotation 1**. This is a prediction, not a verification — still confirm it in
`btime_host` by rendering both candidates and checking the invariant that
actually holds (*the top of the game's picture must land on the right-hand
side of the DVI framebuffer*) before flashing.

### 5.8 Cost estimate

Per native row: 30 char cells (30 strided VRAM reads, 240 pixels), up to 8×16
sprite pixels, and when the background is on another 240. Call it ~600 pixel
writes plus a 320-entry stretch pass, times 240 rows — the same order as
Pac-Man's 288×224 and comfortably under Donkey Kong's per-scanline sprite
search. No reason to expect a budget problem, but measure `work` rather than
assume (`DEVNOTES.md` #25/#35).

---

## 6. Sound

### 6.1 The two AY-3-8910s

Both at **1.5 MHz** (12MHz/8), `AY8910_DISCRETE_OUTPUT`, with per-channel load
resistors — AY1 `{5k, 5k, 5k}`, AY2 `{1k, 5k, 5k}` (btime.cpp:2324-2337).

The chip itself is standard and small. From `ay8910_device::sound_stream_update()`
(ay8910.cpp:1058) and `m_channel = stream_alloc(0, m_streams, master_clock / 8)`
(ay8910.cpp:1298), the model MAME uses is: run at **clock/8 = 187.5 kHz**, and
per step

- each tone channel counts up and toggles on reaching its 12-bit period →
  `f = clock / (16 × period)`;
- the noise counter reaches its 5-bit period (`m_regs[AY_NOISEPER] & 0x1f`),
  toggles a ÷2 prescaler, and clocks the LFSR on alternate toggles → LFSR rate
  `= clock / (16 × period)`. The LFSR is **17 bits, feedback = bit0 XOR bit3,
  output = bit0, seeded to 1 at reset** (`noise_rng_tick()`, ay8910.h:263,
  whose comment notes this was verified against real AY-3-8910 and YM2149
  parts);
- each envelope counts to `period × 2` (`m_step == 2` for a real AY, per
  ay8910.cpp:1576) and advances one of 16 levels → a full envelope sweep is
  `clock / (256 × period)`;
- per channel the mix is `(tone | tone_disable) & (noise | noise_disable)`,
  so **both disabled outputs 1, not 0** — a channel can be played purely by
  modulating its volume, and MAME's comment calls this out because emulators
  get it wrong.

Amplitude: MAME builds its 16-step DAC table from a resistor model
(`build_single_table()` with `ay8910_param`, ay8910.cpp:820/1227). **The port
did better than this section proposed:** rather than substituting a generic
logarithmic table, that model was simply evaluated offline for this board's
two load resistances (5 kΩ on five channels, 1 kΩ on 2A) and the resulting
values baked in as constants — exact, and no more expensive at runtime. Note
that the levels do NOT start at zero, because they are voltage-divider
ratios; the 10 kΩ/10 µF high-pass is what removes that DC. See
`btime_audio.cpp`.

Practical shape: 187.5 kHz ÷ 22050 Hz = 8.503 steps per output sample, so
accumulate and average ~8.5 steps per sample (a box filter is enough
anti-aliasing here and costs one add). Six tone channels, two noise
generators, two envelopes total.

### 6.2 The discrete network — one filter is all that matters

`DISCRETE_SOUND_START( btime_sound_discrete )` (btime.cpp:2216) is unusually
tractable for this project:

- Channels **1A, 1B, 1C, 2B, 2C** are summed and scaled by 0.2. That is it.
- Channel **2A alone** goes through a one-pole **band-pass op-amp filter**
  (`DISC_OP_AMP_FILTER_IS_BAND_PASS_1M`) with `R51 = 5k` (a documented hack —
  the real value is ~1k, but 1k gives a gain of 23.5, so MAME uses 5k and says
  so), `R50 = 10k`, `R49 = 47k`, `C = C = 0.068 µF`, ±5 V rails.
- Then a 2-input op-amp mixer (`100k`/`100k`, `Rf = 10k`, `C = 150 pF`), a
  `10k`/`10 µF` high-pass, and a `3Ω`/`100 µF` speaker high-pass.
- The `µPC1181H` amplifier is explicitly **not** modelled.

So: implement 2A's band-pass as a biquad from those component values, sum the
other five, apply the two high-passes (both are trivially cheap one-poles),
and be done. This is materially less approximation than Donkey Kong or Galaga
needed. The driver even records *why* the levels are what they are, citing two
1982 recordings where the filtered channel is far louder than the music and a
later one where it is closer — worth knowing before "fixing" the balance by
ear, and a reminder of `DEVNOTES.md` #46/#47 (derive the pitch, measure the
shape; and gameplay captures answer mechanism while isolated recordings answer
timbre).

R-values, all measured on a real PCB by "Anoid" per the driver comment
(btime.cpp:2184-2211): `R49 = 47k`, `R50 = 10k`, `R51 = 5k` (hack), `R52 = 1k`.

### 6.3 Main CPU ↔ sound CPU, and the two interrupts

Three separate mechanisms, each easy to get subtly wrong:

1. **The latch and its IRQ.** Main CPU writes `0x4003` → `generic_latch_8`
   → `data_pending_callback().set_inputline(m_audiocpu, 0)`. From
   `gen_latch.cpp:119`, `read()` on a latch with no separate acknowledge does
   `set_latch_written(false)` — so the sound CPU's read of `0xA000` is what
   **clears** the IRQ. Model it as a *level*: asserted from write until read.
2. **The timer NMI.** `TIMER(config, "8vck").configure_scanline(FUNC(audio_nmi_gen), "screen", 0, 8)`
   with `audio_nmi_gen()` doing `m_audionmi->in_w<1>((scanline & 8) >> 3)`
   (btime.cpp:1028), merged by `INPUT_MERGER_ALL_HIGH` with an enable bit.
   So the NMI line is high for 8 scanlines and low for 8 → one rising edge
   every 16 scanlines = **976.56 Hz**. **This must be edge-triggered**, as a
   real 6502 NMI is; treat it as a level and it will re-fire for 8 straight
   scanlines and the sound CPU will do nothing else.
3. **The NMI enable.** btime is `AUDIO_ENABLE_DIRECT` (`init_btime()`,
   btime.cpp:3176), so the enable is **bit 0 of a write to `0xC000-0xDFFF`**,
   not the AY port-A route that Zoar and Lock'n'Chase use
   (`audio_nmi_enable_w` vs `ay_audio_nmi_enable_w`, btime.cpp:1013-1026). It
   starts **disabled** (`machine_reset()` does `in_w<0>(0)`), so the ROM must
   turn it on — a port that hardcodes it on will run the sound CPU's tick
   before the ROM is ready for it, and one that never implements the write
   will produce total silence.

### 6.4 Where the audio work goes in the frame

Follow `dkong_audio_run_slice(i, n)`: generate into a FIFO in slices *inside*
the scanline loop, never in one burst at the end. `DEVNOTES.md` #48 is the
sharpest lesson in the whole file — Donkey Kong's frame had its CPU carefully
interleaved and then a 2.9 ms un-interleaved audio burst bolted onto the end,
and 9.4 + 2.9 ms of a 16.66 ms budget still broke pacing outright. The rule is
not "is there budget" but "is there ever a gap longer than ~2 ms between two
scanline submissions".

Convenient here: the sound CPU is exactly **32 cycles per game scanline**, so
slicing it by scanline is both the natural interleave *and* the thing that
keeps its AY register writes correctly placed in time relative to its own NMI.

---

## 7. Frame loop

One counter can drive everything, which is worth doing deliberately because
all three consumers derive from the same real hardware signal:

```
for each of the 272 game scanlines s:
    main CPU  += 96 cycles   (interleaved, elapsed-delta compare, uint32_t)
    sound CPU += 32 cycles
    vblank bit = (s < 8 || s >= 248)          → read back at 0x4003 bit 7
    if (s & 8) rising edge and nmi_enabled → sound CPU NMI (edge!)
    generate this scanline's AY samples
    on the ~240 scanlines that map to a submitted DVI row: render + submit
```

The 272-vs-240 mismatch is bookkeeping, not a problem: 240 of the 272 game
scanlines are visible, and the submitted DVI scanline count is also 240, so
visible game line ↔ submitted row is 1:1 with the 32 blanking lines carrying
CPU cycles and audio but no output.

Use elapsed-cycle subtraction against a free-running `uint32_t`, never an
absolute target (`DEVNOTES.md` #22, and #26/#27 on never using `long`).

**One honest wart to decide about.** The DVI frame is 60 Hz; Burger Time's is
57.4449 Hz. One game frame per DVI frame therefore runs the whole machine
**4.45% fast** — game speed, animation and music tempo alike. That is the
largest such mismatch in this project (Pac-Man and Donkey Kong are ~1% and
`invaders_machine.cpp` knowingly runs 0.83% fast). Options, in order of
increasing effort:

1. Ship it, note it, listen to it. 4.45% is about three-quarters of a
   semitone of tempo — possibly audible against a recording, probably not
   objectionable in play.
2. Repeat one game frame every ~23.5 DVI frames (emit no game vblank on that
   frame), giving 57.4 game frames/sec at the cost of one duplicated frame
   every 0.4 s.
3. Decouple emulated time from the display entirely and drive the scanline
   counter from `micros()`. Most faithful, most invasive, and the audio-clock
   lesson already in `DEVNOTES.md` says be careful here.

Recommendation: (1) for first light, and record the measurement rather than
the impression before spending anything on (2).

---

## 8. Input and DIP switches

`INPUT_PORTS_START( btime )` (btime.cpp:1263). **All active low** (unlike
Donkey Kong), so bytes are built by masking bits *out* of `0xFF`:

| Port | Bit | Signal |
|---|---|---|
| `P1` `0x4000` | 0/1/2/3 | right / left / up / down (4-way) |
| | 4 | BUTTON1 (pepper) |
| `P2` `0x4001` | same | cocktail player 2 |
| `SYSTEM` `0x4002` | 0/1 | start1 / start2 |
| | 2 | tilt |
| | 6/7 | **coin1 / coin2 — ACTIVE HIGH**, and each triggers the main CPU IRQ |
| `DSW1` `0x4003` | 0-1 / 2-3 | coin A / coin B |
| | 4 | **"Leave Off" — must read 1 or the game locks up at boot** |
| | 5 | unused (reads 1) |
| | 6 | cabinet (0 = upright) |
| | 7 | **vblank** (see §5.6) |
| `DSW2` `0x4004` | 0 | lives (1 = 3, 0 = 5) |
| | 1-2 | bonus life (`0x02` = 20000 default) |
| | 3 | enemies (1 = 4, 0 = 6) |
| | 4 | end-of-level pepper (0 = yes) |
| | 5-7 | unused, all read 1 |

Computed MAME defaults: **DSW1 = `0x3F`** (upright, bit 7 replaced by live
vblank), **DSW2 = `0xEB`**.

Two boot traps in there. DSW1 bit 4 is labelled *"Must be OFF. No test mode in
ROM, so this locks up the game at boot-up if on"* — and note "off" for an
active-low switch means the bit **reads 1**. And DSW2's top three bits read 1,
which is the `PORT_DIPUNUSED`-is-not-zero trap (`DEVNOTES.md` #24) waiting to
happen again.

The coin bits being **active high in an otherwise active-low port** is worth a
comment in the code; it is exactly the sort of thing that gets normalized away
by accident.

Fruit Jam button mapping — every signal the game needs already exists in
`board_config_fruitjam.h` (`COIN`, `START1`, `START2`, `UP`, `DOWN`, `LEFT`,
`RIGHT`, `SHOOT`, plus `ROTATE`/`MIRROR`), with `SHOOT` as pepper. No new
board work at all.

---

## 9. What gets built

| New | Notes |
|---|---|
| `libraries/ArcadeCPU_M6502/` | `m6502.c`/`.h`, plain C. Vendored MIT core + an added `read_opcode` hook for §4.2. Credit in README. |
| `libraries/ArcadeMachine_BTime/` | `btime_machine`, `btime_ports`, `btime_video`, `btime_audio`, `btime_input`, `btime_assets` — the same six-file shape as `ArcadeMachine_DKong`. |
| `btime_fruitjam/` | `.ino`, `README.md`, `sketch.yaml` (start at `opt=Optimize3`; `-Os` is not fast enough for anything here — README's own warning). |
| `tools/btime_host/` | `build.sh` + `main.cpp`, copied from `dkong_host` with the CPU list changed. **Build this first.** |

Nothing in `ArcadeBoard_FruitJam` or `ArcadeHAL` needs to change.

## 10. Order of work

Straight from the playbook, which reached working hardware with zero debug
cycles on Ms. Pac-Man when followed strictly:

1. ✅ **Verify the ROM set against MAME's CRC32s.** Done — §1.
2. `ArcadeCPU_M6502` + the CPU-7 hook, with Klaus's functional/decimal tests
   in the harness. A CPU core is the one component where a bug looks like
   *every* other kind of bug, so buy certainty here first.
3. `tools/btime_host/` with a stub renderer: get the machine to boot, and
   assert on the two things that will otherwise waste a hardware cycle — that
   the vblank bit is being polled (count reads of `0x4003`), and that the
   opcode-scramble path actually fires (count descrambled fetches; if it is
   zero, either the mask is wrong or nothing has written yet, and both matter).
4. Video: characters → palette → sprites → background, each verified as a PPM
   in the harness before the next. Run `pacman_host` or `dkong_host` as a
   **control** before believing any "garbled frame" (an attract screen is not
   a broken decode — that control cost one command and saved a whole
   investigation on Ms. Pac-Man).
5. Confirm rotation in the harness against the framebuffer invariant (§5.7),
   *then* flash.
6. Hardware: video, then input, then audio — each separately.
7. Audio last: AY cores → the 2A band-pass → the mixer/high-passes. Compare
   the whole pitch contour against a recording, not a mean (`DEVNOTES.md` #46).
8. Add the asset-failure diagnostic pattern from `dkong_fruitjam` (report once
   per second from `loop()`, name the missing files) — new sketches should
   start with it rather than acquire it after a silent boot failure.

## 11. Traps, ranked for this machine specifically

1. **The polled vblank bit** (§5.6). No interrupt exists; a constant hangs the
   game. First thing to instrument, first thing to suspect.
2. **The CPU-7 flag is "any write since the last opcode fetch"**, not "the
   previous bus cycle" — and interrupt pushes count (§4.2).
3. **Sound NMI is edge-triggered** and starts disabled (§6.3).
4. **DSW1 bit 4 must read 1** or the game locks at boot; DSW2's unused bits all
   read 1 (§8).
5. **The `maincpu` load order is not filename order**, and `ab03.6b` is a
   tilemap, not graphics (§1).
6. **Sprite attributes are interleaved at stride `0x20` inside video RAM**, and
   both coordinates are `240 - value` with a Y adjust of 1 (§5.2).
7. **65C02-vs-NMOS divergences** — settle by census, not by inspection (§4.1).
8. **Never run an un-interleaved burst between scanline submissions**, audio
   included (§6.4). Five separate DEVNOTES entries; do not make it six.
9. **The 4.45% frame-rate mismatch** is real and larger than any previous
   game's (§7). Measure it; don't argue about it.

## 12. Open questions

- ~~Does anything in `btime` actually read the X/Y-swapped *mirror* windows,
  or only write them?~~ **ANSWERED: it reads them.** Zero reads during
  attract, ~11,000 per 900 frames once a game is running. The read path is
  load-bearing.
- `B000-BFFF` is mapped as ROM but unpopulated in this set. What does the real
  board return, and does the program ever read it? **Still open.** The port
  returns 0. `DEVNOTES.md` #24 is a standing warning that open-bus reads can
  be load-bearing.
- ~~Is the background layer used in normal play, or only on particular
  screens?~~ **ANSWERED: it is the level playfield.** `bnj_scroll0` goes to
  `0x13` when a level starts (bit 4 enable, coarse scroll 3), so the layer is
  not an optional extra.
- ~~Which AY channel is 2A in practice (the filtered one)?~~ **ANSWERED by
  derivation rather than by ear:** the band-pass works out to a ~187 Hz peak
  with Q ≈ 1.9 and 7× gain, so 2A is the bass/thump channel — and its 1 kΩ
  load resistor (against 5 kΩ on the other five) independently confirms it is
  the odd one out. See `DEVNOTES.md` §55.

New open questions the implementation raised, all of them needing hardware:

- Is rotation 1 right on the physical display? (Predicted from MAME's
  `ROT270`, confirmed in the harness against the framebuffer invariant, but
  seven-for-seven is a rule, not a measurement of *this* board.)
- 1:1 or the aspect stretch (§5.7)?
- Is `OUTPUT_GAIN` sensible, and does leaving out the cabinet's 530 Hz speaker
  high-pass make the port boomier than a real machine (§6.2, `DEVNOTES.md`
  §55)?
- ~~What does `work` measure per frame, and does the no-decode-cache trade
  (`DEVNOTES.md` §54) need revisiting?~~ **ANSWERED, and the trade was
  wrong.** The first flash needed 23.6ms and went red; decode caches for
  chars and background tiles were the first fix of six. Final `work` is
  11.6–16.4ms. See §59–64, and note §54 is now superseded by §59.
