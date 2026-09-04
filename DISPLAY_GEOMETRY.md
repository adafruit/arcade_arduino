<!--
SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
SPDX-License-Identifier: MIT
-->

# Display geometry: aspect ratio, rotation, and the landscape red screen

Status: **analysis only — no renderer has been changed yet.** Written after
auditing `hal_video_fruitjam.cpp`, `arcade_hal_video.h`, all seven
`*_video.cpp` renderers, the vendored `libdvi`, the `tools/*_host`
harnesses, and both DEVNOTES files (this one and `invaders_pico`'s).

Every claim below was checked against source, not inferred. Where this
document contradicts an existing comment in the tree, the contradiction is
called out explicitly rather than silently corrected, because the existing
comments were right when they were written and the reason they stopped
being right is the actual lesson.

---

## 1. The board is a 320x240 canvas of square pixels

Not a 640x480 framebuffer. Both axes are doubled:

- **Vertically** by `dvi_vertical_repeat = 2` — known, documented, and the
  subject of DEVNOTES #8-#10.
- **Horizontally** by libdvi itself, which is the half that never made it
  into the HAL. `dvi_config_defs.h:54` defaults `DVI_SYMBOLS_PER_WORD` to 2,
  and `_dvi_prepare_scanline_16bpp()` (`src/libdvi/dvi.c:141-143`) passes
  `pixwidth / 2` as the source pixel count to `tmds_encode_data_channel_16bpp()`.
  With `h_active_pixels = 640` that is **320 source pixels**, doubled across
  the 640-pixel line.

So bytes 320..639 of every scanline buffer are read by nothing, ever, and
one logical pixel is a 2x2 block of physical pixels. On a 4:3 panel that
makes logical pixels **square** (320/240 = 4/3).

This is already recorded in prose in several places — `tools/README.md`'s
"PPM dumps and the half-width buffer", `btime_video.cpp`'s geometry
comment, and every `TATE_BX`/`LAND_BX` constant written as `(320 - N) / 2`.
What it is *not* recorded in is the HAL contract, which still declares
`HAL_VIDEO_WIDTH = 640` and still hands renderers a `dvi_y` in 0..479 of
which only even values ever arrive. **The project carries two coordinate
systems for one canvas: x in 320-space, y in 480-space.** Section 4 is what
that costs.

### Side effects of the undeclared horizontal halving

- Invaders, Pac-Man, Ms. Pac-Man and DKong `memset()` 640 `uint16` per
  scanline. Half of that — 640 bytes x 240 rows = 153KB per frame,
  **9.2MB/s** — is stores into memory the display never reads, on the same
  SRAM bus Core 1's DVI DMA is competing for. Galaga's border clear runs to
  640 too. BTime is the only renderer that already stops at 320
  (`VISIBLE_COLS`), and its comment says why.
- The 8 scanline buffers are allocated 640 `uint16` wide when 320 is what
  libdvi reads: 5,120 bytes of dead SRAM.

---

## 2. Aspect ratio: we do not match the original, in either mode

### The target

The canvas is 320x240 square pixels on a 4:3 panel. A real rotated-monitor
(ROT90/ROT270) cabinet fills its tube edge to edge, so:

- **Tate** (monitor physically rotated): the game should fill the whole
  canvas — **320 x 240**, no borders.
- **Yoko** (monitor left in landscape, portrait game shown upright inside
  it): the game's long axis is capped at 240 canvas rows, so an
  aspect-correct width is 240 x 3/4 = **180 columns**. The picture should be
  **180 x 240**, pillarboxed 70 columns each side.

### What we actually draw

| game | raster (long x short) | tate now | tate target | yoko now | yoko target |
|---|---|---|---|---|---|
| Invaders / Lunar Rescue / DKong | 256 x 224 | 256 x 224 | 320 x 240 | 224 x 240 | 180 x 240 |
| Pac-Man / Ms. Pac-Man / Galaga | 288 x 224 | 288 x 224 | 320 x 240 | 224 x 240 | 180 x 240 |
| Burger Time | 240 x 240 | 240 x 240 | 320 x 240 | 240 x 240 | 180 x 240 |

Resulting distortion, expressed as **how much too wide the picture is for
its height**:

| game | tate | yoko |
|---|---|---|
| Pac-Man / Ms. Pac-Man / Galaga | +3.7% | +24.4% |
| Invaders / Lunar Rescue / DKong | +16.7% | +24.4% |
| Burger Time | +33.3% | +33.3% |

Pac-Man and Galaga come out nearly right in tate **by luck**: 288:224 is
close to 4:3, so their real pixels were nearly square. Invaders' pixels were
genuinely tall and narrow (0.857 w:h) and Burger Time's raster is square, so
both need real stretching and get none.

### Why: there is no scale-to-screen step anywhere

The renderers lay the picture out in *native pixel counts* and pillarbox the
remainder. `TATE_BX = (320 - 256) / 2` is a **centring** constant, not a
scaling one. The only scale factor in the entire project is
`btime_video.cpp`'s `g_aspect_stretch` (`nx = col * 3 / 4`, filling all 320
columns), which is correct, is exactly the right shape for the general fix
— and is **off by default**, in one game.

The nice part: that fix generalises with no division in any loop, because
every ratio is exact at 320 columns. 240 -> x3/4, 256 -> x4/5, 288 -> x9/10.

### Corroboration that the 4:3 model is right

`invaders_pico`'s DEVNOTES section 7 measured the shield on the physical
display at **5.6mm in yoko vs 6mm in tate**, a 6% difference, and called it
imperceptible. The model above predicts that ratio independently: the
256-axis gets 3.20 screen units in tate and 3.00 in yoko, = 0.9375. The
measurement was 0.9333. That match is the reason to trust the table.

What the measurement did *not* establish is that either mode was correct.
It compared yoko against tate, and tate was already 16.7% off.

---

## 3. The x1.875 landscape stretch: right answer, then invalidated underneath

`invaders_video.cpp`'s header comment says DVI x pixels are "2x the physical
size of DVI y pixels" and that game rows are "stretched x1.875 into DVI y to
compensate."

**That premise was TRUE when it was written.** `invaders_pico` ran
`dvi_vertical_repeat = 1` (overridden in its CMake build via
`target_compile_definitions(libdvi INTERFACE DVI_VERTICAL_REPEAT=1)`), so
its canvas was 320 x 480 — genuinely 2:1 pixels. Its DEVNOTES section 7
records the hardware measurement that proves the fix worked: the shield went
from 3mm to 5.6mm against tate's 6mm. A real 2x correction, measured, not
inferred.

**What DEVNOTES #10 changed is the sample count, not the geometry.** After
switching to `dvi_vertical_repeat = 2` and 240 submissions per frame, the
formula still lands the image across all 480 *physical* rows — the picture
is the same size it always was. What changed is that it now takes **240
samples of a 256- or 288-entry axis instead of 480**:

```
dy = dvi_y * GAME_WIDTH / HAL_VIDEO_HEIGHT     with dvi_y in {0,2,4,...,478}
```

| game | source cols | canvas rows | dropped | pattern |
|---|---|---|---|---|
| Invaders / Lunar Rescue / DKong | 256 | 240 | 16 | every 16th |
| Pac-Man / Ms. Pac-Man / Galaga | 288 | 240 | 48 | every 6th |
| Burger Time | 240 | 240 | 0 | 1:1 |

A lossless 1.875x **upsample** became a lossy 0.9375x **downsample** that
silently drops whole native lines on an uneven truncating-division schedule.
That is DEVNOTES #23's irregular test grid, and it is not Pac-Man-specific —
it is the shared formula, in every game except Burger Time.

**#10's own summary is the smoking gun:** *"This preserved every existing
TATE_BY/TATE_BX/LAND_BX calibration constant unchanged."* True of the border
constants, which are positions. False of the `/ HAL_VIDEO_HEIGHT` divisor,
which is a **resampling ratio** whose meaning changed underneath it. Nothing
in the type system or the contract could have caught that, because the HAL
declares one axis in 320-space and the other in 480-space.

**Lesson worth keeping:** a change to the sample rate is not a change to the
geometry, and constants that survive one are not automatically valid under
the other. Positions transferred; ratios did not.

---

## 4. Why landscape is hard to switch into, and why it goes red

One structural decision, upstream of every landscape symptom.

The machine layer's only compositing primitive is `render_native_row()`, and
the HAL's only output primitive is "the next physical scanline." **In tate
those two coincide, so everything works.** In yoko a physical scanline is a
native *column*, so the mismatch has to be bridged — and every tile/sprite
game bridges it the same way:

- a full-frame cache built in one uninterrupted burst
  (`frame_cache[224][288]` = 129KB in Pac-Man, Ms. Pac-Man and Galaga;
  `[224][256]` = 115KB in DKong; BTime's 8-bit `frame_pen[240][240]` =
  57.6KB), **plus**
- `run_frame_sequential()` running the whole frame's CPU cycles in another
  burst,

both *before* the frame's first `hal_video_acquire_scanline()`.

That is the exact thing the framework forbids everywhere else — #18, #20,
#34, #36 and #48 are all the same lesson — and the consequence is
mechanical, not mysterious:

- Core 1's cover is 8 scanline buffers (~555us, and 8 is a **hard** ceiling
  per #9, not a tunable) plus ~1.4ms of vblank. Call it **~2ms**.
- The bursts are **9-18ms**. DKong measures 9.3ms silent and 11.6-13.6ms
  with sound, peaking at 15.8ms.
- `dvi.c`'s DMA IRQ handler has an explicit branch with its own comment:
  *"No valid scanline was ready (generates solid red scanline)."*

Red for as long as the burst lasts, which at those numbers is most of the
screen. #59's reading applies directly: this is not a rendering bug, it is
the frame not being produced in time.

**Space Invaders and Lunar Rescue are exempt for a reason that has nothing
to do with their landscape code being better.** Their video hardware is a
1-bit column-major bitmap in CPU RAM, so `vram[dx*32 + (col>>3)]` costs the
same iterated along either axis. No compositing step, therefore no cache,
therefore no burst, therefore all four rotations interleave.

Note the caches are `static` arrays in BSS: **they cost their full 57-129KB
in every build, in every orientation, including tate, where they are never
read.**

### Two things that make this worse than it needs to be

1. **The row-only primitive.** There is nothing wrong with the *idea* of a
   column slice — on a tilemap machine it is one tile column x 28 tile rows
   plus the sprites overlapping that x, the same order of work as a row
   slice with an x-overlap test instead of a y-overlap test. It simply was
   never written, so landscape had to buy a 129KB frame buffer instead.
2. **The two coordinate systems.** Every rotation formula silently carries a
   x2 or /2 to bridge x-in-320-space against y-in-480-space. Section 3 is
   what that cost once already.

### An inconsistency in the record

DEVNOTES stated DKong's *"yoko orientations were reported working rather
than showing the red lines problems #18/#19/#20 predict for the sequential
path — consistent with its lighter per-frame budget."*

But `dkong_machine.cpp:152` gates `run_frame_interleaved()` on
`rotation == 1 || rotation == 3`, and `dkong_video.cpp:424` builds the same
`frame_cache`, so structurally DKong should be **as red as Ms. Pac-Man**. At
9.3ms of work (13.6ms with sound) against ~2ms of cover, "lighter budget"
does not close that gap — DKong is the second-heaviest game in the project.

**Measured, not argued.** `hal_video_take_starve_count()` was wired into the
sketch heartbeat and each game booted straight into one rotation via a
`-DTEST_ROTATION=n` build flag:

| game / rotation | work | frame | starve / 60 frames |
|---|---|---|---|
| Lunar Rescue, rot 1 (tate) | 3.1-3.4ms | 17.3ms | **4-5** |
| Lunar Rescue, rot 0 (landscape) | 4.9-5.3ms | 17.3ms | **5** |
| DKong, rot 1 (tate) | 11.7-13.7ms | 16.66ms, pinned | **0** |
| DKong, rot 0 (landscape) | 12.4-14.7ms | 15.6-17.9ms, jittery | **120-123** |

The report was wrong, and the retraction is DEVNOTES #75. DKong's landscape
starves the queue about twice per frame, every frame; its tate path starves
zero times. Lunar Rescue is the control and confirms the other half: its
landscape is *exactly* as clean as its tate, so the 4-5/60 floor present in
both is the residual #16 bus-contention effect, not an orientation effect.

Read the counter correctly: ~2 events per frame is the **saturation value
for one burst per frame**, not "two bad scanlines." No submits happen during
the 12.5ms burst, so nothing is counted while the queue sits empty and Core
1 paints red; the two events are the first two submits *after* the burst.
The red is the ~180 scanlines in between.

---

## 5. Geometry facts worth stating plainly

These belong in the record as facts about the hardware, not as preferences.

**Tate is the supported mode for a geometric reason, not a taste one.**
Rotated, the canvas offers 240 x 320 = **77k pixels** for a game with 57-64k
— every native pixel survives, and the picture is *upsampled*. Un-rotated,
the aspect-correct window is 180 x 240 = **43k pixels** for the same game.
**There is no way to avoid throwing pixels away in yoko: 20-37% of them,
inherently, at any quality of implementation.** That asymmetry is the real
argument for tate, and it is worth writing down so nobody re-litigates it as
a rendering-quality problem that better code could fix.

**Landscape needs the picture to get NARROWER, not taller.** This is the
counterintuitive part and the one most likely to be got backwards. A 3:4
portrait image inside a 4:3 landscape canvas at full height is **180 canvas
columns wide, not 224**. The current code draws it 224 wide, which is why
yoko is 24.4% too wide in every game. The fix downsamples the player's
horizontal axis 224 -> 180 while keeping all 240 rows.

**Every scaling ratio in this project is exact at 320 columns**, so the
aspect fix needs no division in any inner loop — a 320-entry LUT built at
init, or Bresenham, covers all seven games: 240 -> x3/4, 256 -> x4/5,
288 -> x9/10.

**The 8-buffer scanline queue is ~555us of cover, and it cannot be raised**
(#9: `dvi_init()` hardcodes 8). Any renderer design that needs more slack
than that has to get it by *not bursting*, not by buffering.

---

## 6. What the host harness cannot see

`dump_ppm()` in every `tools/*_host/main.cpp` loops `y = 0 .. HAL_VIDEO_HEIGHT-1`
and renders at **every** physical row — 480 samples per frame. The device
submits 240. So the harness renders landscape at the *pre-#10* sample rate,
which is the lossless one.

**The harness renders landscape the good way and structurally cannot
reproduce the dropped-row bug at all.** Any geometry work validated against
current PPM dumps is validated against a model that does not contain the
defect. This must be fixed before, not after, the renderers change — and
fixing it invalidates every stored byte-compare baseline, which will need
regenerating.

(The harness also cannot see red: `hal_host.cpp` returns 0 from
`hal_video_take_starve_count()` by design, and says so. That part is
already documented and correct — starvation is device-only.)

Separately: `hal_video_take_starve_count()` was **declared in the HAL,
implemented in the board backend, and called from nowhere** — the project's
one instrument for the one failure mode invisible to every other instrument
was dead code. Wiring it into the sketch stats line is what made section 4's
DKong question answerable.

---

## 7. Plan

Ordered by leverage. Phases 0 and 1 are what make everything after them
verifiable.

**Phase 0 — make the harness honest.** Change `dump_ppm()` to loop
`HAL_VIDEO_SCANLINES_PER_FRAME` and emit what the monitor actually shows.
Regenerate the byte-compare baselines.

**Phase 1 — one coordinate space.** `HAL_VIDEO_WIDTH` -> 320,
`HAL_VIDEO_HEIGHT` -> 240, `dvi_y` becomes 0..239, and every x2 and /2
disappears. Make the HAL contract say what the hardware is. Mechanical
sweep of ~30 files; the output must be pixel-identical apart from the
intended change. This makes the section 3 class of bug unrepresentable, and
it ends the 640-wide `memset` for free.

**Phase 2 — one shared geometry module.** `ArcadeHAL/src/arcade_video_geom.h`,
with `av_geom_init(long_px, short_px)` building four Bresenham LUTs (tate:
320 cols -> long axis, 240 rows -> short axis; yoko: 240 rows -> long,
180 cols -> short). Every game's `TATE_BX`/`LAND_BX` and every
`dvi_y * W / H` becomes a lookup. Put it behind a runtime flag defaulting
off, so the hardware A/B is one button press — the same discipline
`btime_video.cpp` already uses.

> **Budget risk, must not be skipped.** Tate's 224 -> 240 upsample
> duplicates 16 rows, i.e. 16 extra `render_native_row()` calls per frame
> (+6.7% render). DKong peaks at 15.8ms of a 16.66ms budget with sound.
> Memoize — `if (lut[i] == lut[i-1])` reuse the last rendered row — or this
> phase puts DKong back into red. Phase 1's dropped `memset` pays some of
> it back.

**Phase 3 — `render_native_column()`, which deletes the red at its source.**
Landscape and 180 then interleave exactly like tate; `frame_cache` and
`run_frame_sequential()` both go away, freeing 57-129KB. Much cheaper than
the double-buffering redesign #20 floats, which is off the table anyway at
129KB per buffer. Galaga needs one extra piece — an x-bucketed star list —
but it already precomputes hit positions rather than walking the LFSR per
pixel, so that is a re-bucketing, not new emulation. Do Pac-Man first (it
has a host harness and the smallest cached renderer), confirm on hardware,
then port the pattern.

> **Tradeoff worth naming:** column-order CPU interleaving shears the image
> instead of tearing it, because a native column is sampled across the whole
> frame's CPU time rather than one row's. At ~1-2px/frame object motion
> across a 16px sprite that is ~0.1px. Invisible, but it should be a
> recorded decision rather than a surprise.

**Phase 4 — collapse the 32 hand-derived rotation cases** into one transform
parameterised by `{swap_axes, flip_x, flip_y}` plus the per-axis scale LUT.
Four cases x eight games, each verified separately, is what produced #21
(mirror instead of rotation), #33 (default 180 off) and #23. All three are
the same failure: a case derived from a sibling case rather than from one
source of truth. Do this **last**, only once phases 1-3 are confirmed on
hardware. It also retires #33's rule that "any default rotation must have a
fast path" — with one code path there is nothing to special-case.

**Phase 5 — cleanups.** Shrink the scanline buffers 640 -> 320 (5KB). Wire
`hal_video_take_starve_count()` into every sketch's stats line (done for
`lrescue_fruitjam` and `dkong_fruitjam` during the section 4 measurement).
