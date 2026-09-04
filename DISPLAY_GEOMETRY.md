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

This was already recorded in prose in several places — `tools/README.md`,
`btime_video.cpp`'s geometry comment, and every `TATE_BX`/`LAND_BX` constant
written as `(320 - N) / 2`. What it was *not* recorded in was the HAL
contract, which declared `HAL_VIDEO_WIDTH = 640` and handed renderers a
`dvi_y` in 0..479 of which only even values ever arrived. **The project
carried two coordinate systems for one canvas: x in 320-space, y in
480-space.** Section 4 is what that cost.

**FIXED in phase 1.** `HAL_VIDEO_WIDTH` and `HAL_VIDEO_HEIGHT` are now the
canvas (320x240), `dvi_y` runs 0..239, `HAL_VIDEO_SCANLINES_PER_FRAME` is
gone, and every x2 and /2 with it.

### Side effects of the undeclared horizontal halving

- Invaders, Pac-Man, Ms. Pac-Man and DKong `memset()` 640 `uint16` per
  scanline. Half of that — 640 bytes x 240 rows = 153KB per frame,
  **9.2MB/s** — was stores into memory the display never reads, on the same
  SRAM bus Core 1's DVI DMA is competing for. Galaga's border clear ran to
  640 too. BTime was the only renderer that already stopped at 320, and its
  comment said why.

  **Fixed for free by phase 1**, since these all clear `HAL_VIDEO_WIDTH`.
  Measured on hardware, Donkey Kong in tate with sound:

  | | before | after |
  |---|---|---|
  | `work` | 11.65-13.75ms | **11.18-13.30ms** |
  | `work_max` | 15803us | **15481us** |

  ~450us a frame, ~2% of the 16.66ms budget, for deleting stores nothing
  ever read.

- The 8 scanline buffers are still allocated 640 `uint16` wide when 320 is
  what libdvi reads: 5,120 bytes of dead SRAM. Deliberately left until every
  renderer is known to stop at 320 — phase 5.

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

| game | source cols | canvas rows | dropped | spacing of the drops |
|---|---|---|---|---|
| Invaders / Lunar Rescue / DKong | 256 | 240 | 16 | exactly every 16th |
| Pac-Man / Ms. Pac-Man / Galaga | 288 | 240 | 48 | exactly every 6th |
| Burger Time | 240 | 240 | 0 | 1:1, nothing dropped |

A lossless 1.875x **upsample** became a lossy 0.9375x **downsample** that
silently drops whole native lines. It is not Pac-Man-specific — it is the
shared formula, in every game except Burger Time.

**The drops are evenly spaced, and that is worth being precise about**,
because it is tempting (and wrong) to describe truncating division as
producing a ragged pattern. Because the submission count is exactly half the
physical height, the ratios come out exact — 256/240 = 16/15 and
288/240 = 6/5 — so the dropped lines land on a perfectly regular stride,
verified by enumerating the formula over all 240 device samples. As a
nearest-neighbour downscale this is the best-behaved case there is.

**The defect is not raggedness, it is that 16 or 48 whole raster lines are
never drawn at all.** Any feature one pixel thin — a maze wall, a bullet, a
character's outline — vanishes completely when it lands on a dropped line,
and reappears when it moves one pixel. What #23 saw as an "irregular" test
grid is a *regular* drop beating against regularly spaced content: a grid
whose pitch is not coprime with 6 has some cells narrowed and others not,
which reads as irregularity even though the sampling is uniform.

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

## 6. What the host harness could not see — FIXED (Phase 0)

`dump_ppm()` in every `tools/*_host/main.cpp` looped
`y = 0 .. HAL_VIDEO_HEIGHT-1` and rendered at **every** physical row — 480
samples per frame. The device submits 240. So the harness rendered landscape
at the *pre-#10* sample rate, which is the lossless one:

```
480 samples (old dumper):  288 of 288 source columns drawn,   0 dropped
240 samples (real device): 240 of 288 source columns drawn,  48 dropped
```

**The harness rendered landscape the good way and structurally could not
reproduce the dropped-row bug at all.** Anything validated against those
dumps was validated against a model that did not contain the defect.

**Fixed.** All six harnesses now share `tools/host_common/host_ppm.cpp`,
which renders exactly `HAL_VIDEO_SCANLINES_PER_FRAME` times at the
coordinate the board backend passes, then reproduces both the vertical
repeat and the horizontal doubling on the way out. Output stays 640x480 and
looks like the monitor. Verified on real dumps: adjacent output rows are
identical in pairs and adjacent output columns are identical in pairs, so
the file is provably a faithful 2x blow-up of the 320x240 canvas. Neither
property held before.

Measured off those dumps, Pac-Man at frame 3000 — and note how exactly this
reproduces section 2's table:

| rotation | picture occupies | should be |
|---|---|---|
| 0 (landscape) | logical cols 48..271 = **224** wide x 240 rows | 180 x 240 |
| 1 (tate) | 288-wide field, **224** of 240 rows used | 320 x 240 |

Landscape is 224/180 = **1.244x too wide**, the +24.4% predicted. Tate's
224-of-240 rows against 288-of-320 columns is the +3.7% predicted for
Pac-Man.

This change invalidates every stored byte-compare baseline; `galaga_host`
and `pacman_host` move most, since their old dumps did not horizontal-double
at all.

(The harness still cannot see red: `hal_host.cpp` returns 0 from
`hal_video_take_starve_count()` by design, and says so. That part is correct
— starvation is device-only, which is what section 4's hardware measurement
was for.)

Separately: `hal_video_take_starve_count()` was **declared in the HAL,
implemented in the board backend, and called from nowhere** — the project's
one instrument for the one failure mode invisible to every other instrument
was dead code. Wiring it into the sketch stats line is what made section 4's
DKong question answerable.

---

## 7. Plan

Ordered by leverage. Phases 0 and 1 are what make everything after them
verifiable.

**Phase 0 — make the harness honest. DONE.** All six `dump_ppm()` bodies
replaced by the shared `tools/host_common/host_ppm.cpp`; see section 6 for
what it fixes and the dumps that verify it. Byte-compare baselines need
regenerating.

**Phase 1 — one coordinate space. DONE.** `HAL_VIDEO_WIDTH` -> 320,
`HAL_VIDEO_HEIGHT` -> 240, `dvi_y` runs 0..239,
`HAL_VIDEO_SCANLINES_PER_FRAME` removed entirely, and every x2 and /2 gone
with it. The HAL contract now names the canvas, not the physical
resolution, which makes the section 3 class of bug unrepresentable.

*Verification.* 44 PPM dumps — 6 games x 4 rotations, captured before the
sweep with the phase 0 dumper — came back **byte-identical** afterwards.
That is the whole point of doing phase 0 first: this sweep touched 20 files
and every rotation formula in the project, and "pixel-identical" is a claim
that can be checked rather than asserted. All 8 sketches compile; Donkey
Kong confirmed on hardware in tate (`starve 0/60`, `frame` pinned at
16.66ms) and in landscape (`starve 120/60`, unchanged — phase 1 was not
meant to fix that). Free win measured: see section 1.

*Note on the landscape divisor.* `dy = dvi_y * GAME_WIDTH / HAL_VIDEO_HEIGHT`
did not change, and did not need to: written against `HAL_VIDEO_HEIGHT`, it
gives the same answer at 240 samples of a 240-row canvas as it did at 240
samples of a 480-row one. It is still the wrong *target* (it fills 240 rows
against a 224-column axis instead of 180) — that is phase 2's job, not
phase 1's.

**Phase 2 — one shared geometry module. BUILT; correction stays OFF by
default.** `ArcadeHAL/src/arcade_video_geom.h` owns the geometry for all
seven machines: `av_geom_init(long_px, short_px)` builds `av_tate`/`av_yoko`,
and every per-game `TATE_BX`/`TATE_BY`/`LAND_BX` is gone.
`av_geom_set_stretch()` switches between the historical layout and the
cabinet-correct one.

*Verification.* `tools/geom_test/` asserts both directly: with the
correction off the maps reproduce every historical constant exactly
(Invaders tate `x[32,288)`, Pac-Man `x[16,304)`, `y[8,232)`, yoko
`x[48,272)`, Burger Time `x[40,280)`), and with it on every game lands on
the same destination — `320x240` tate, `180x240` at x0=70 yoko — plus
monotonicity and in-range indices. 40 of the 44 byte-compare dumps were
unchanged; see below for the 4 that were not. With the correction on,
DKong and Galaga fill their windows edge to edge (yoko content exactly
70..249, tate 0..319 x 0..239).

*It found a bug.* Donkey Kong's landscape picture had been 16 canvas pixels
left of centre, because its `LAND_BX` was derived from the LONG axis (256)
while the loop walked the SHORT one (224). Those are the 4 differing dumps,
and the diff is a pure translation. DEVNOTES #77.

> **The budget risk was real, and bigger than predicted — DEVNOTES #78.**
> Measured on DKong in tate with sound: stretch off `work_max` 14809us and
> `starve` 0/60; stretch ON `work_max` **17543us**, `starve`
> **8448-11148/60**, `frame` 18.2ms. Over budget, red, not 60fps.
>
> The predicted cost — 16 duplicated rows, +7% of render — is the *smaller*
> half and is still un-memoised. The larger half was not predicted: the
> per-pixel column map turns a linear copy into a double-indirect
> `buf[x] = row[col[x]]`, and **that cost hit the DEFAULT path first**,
> pushing `work_max` to 17126us with the correction OFF and not one pixel
> changed. A `col_1to1` fast path (linear copy whenever the map is an
> identity shift, as `galaga_video.cpp` already did) restored it to 14809us.
> A lookup table is not free just because it removes arithmetic.
>
**Phase 2b — making the correction affordable. PARTLY DONE; DKong still
does not fit.** DEVNOTES #78 has the full account. Both planned
optimisations landed and neither was the win expected, so the frame was
instrumented instead (`-DDKONG_COST_TRACE=1`, `dkong_debug_take_render()`).

Measured on DKong in tate with sound, per frame:

| | rows rendered | `render_native_row` | **emit** | `work_max` | `starve`/60 |
|---|---|---|---|---|---|
| correction off | 224/224 | 4848-4984us | **687us** | 14751us | **0** |
| correction ON | 224/240 | 4287-4952us | **2660us** | 16877-17010us | 5795-9717 |

**The entire cost of the correction is the emit pass** — 687us at 1:1
against 2660us resampled, on a 16660us budget. Everything else is noise:

- Row memoisation works (224 renders for 240 canvas rows, visible in the
  `rows` column) and is worth ~0.15ms. Kept, but it was never the problem.
- The source-driven emit alone changed nothing measurable. **`rep[]` is
  still a load**, so the loop is two loads and two stores against the old
  two loads and one store — the dependency was removed, but on this core a
  dependent SRAM load was not what cost.
- What did help: **hoisting `m->src_n`, `m->rep` and `m->x1` into locals
  before the loop**, 3.75ms -> 2.66ms (-29%). `dst` is `uint16_t *` and the
  map holds `uint16_t`, so the compiler had to assume a store through `dst`
  could alias the map and reloaded those fields every iteration.

Still ~1.0ms short of fitting. **The remaining lever is to delete the second
pass entirely**, not to speed it up: have `render_native_row()` write
through the column map straight into the scanline buffer, the way
`galaga_video.cpp`'s 1:1 fast path already writes straight into `buf`. That
removes the 687us copy from the DEFAULT path too, so it is worth doing
regardless of the correction. It is invasive — sprites overlay pixels and a
1->2 expansion has to widen those writes as well — which is why it was not
attempted here.

> **A measurement gotcha worth keeping.** The instrument perturbed the thing
> it measured. Two unconditional counter increments per scanline — costing
> nothing detectable in `work` — took the unstretched build from `starve`
> 0/60 to ~1000/60. Both the timers and the counters are now behind
> `DKONG_COST_TRACE`. `starve` tracks intra-frame DISTRIBUTION, not totals
> (#35); anything added to a path that runs 240 times a frame can move it
> while leaving every other number alone.

**Phase 3 — `render_native_column()`, which deletes the red at its source.**
Landscape and 180 then interleave exactly like tate; `frame_cache` and
`run_frame_sequential()` both go away. Much cheaper than the
double-buffering redesign #20 floats, which is off the table anyway at 129KB
per buffer — and note the caches sit in BSS, so today they cost their full
size in EVERY orientation including tate, where they are never read:

| game | frees | globals now | after |
|---|---|---|---|
| Pac-Man | 129KB | 252KB | 123KB |
| Ms. Pac-Man | 129KB | 334KB | 205KB |
| Galaga | 129KB | 299KB | 170KB |
| Donkey Kong | 115KB | 287KB | 172KB |
| Burger Time | 58KB | 266KB | 208KB |

**The per-game difficulty is not uniform, and one fact decides it:** whether
sprite selection is order-dependent per raster line.

| game | difficulty | why |
|---|---|---|
| Pac-Man, Ms. Pac-Man | easy | a column is one tile column x 28 rows; all 8 sprites drawn unconditionally, so it is an x-overlap test instead of a y-overlap test |
| Burger Time | easy | same shape, 8 sprites, 8-bit pen buffer |
| Galaga | moderate | same as Pac-Man, plus the 05XX starfield needs an x-bucketed hit list. It already precomputes hit POSITIONS rather than walking the LFSR per pixel, so that is a re-bucketing, not new emulation |
| **Donkey Kong** | **hard** | `dkong_video.cpp`'s `num_sprt < 16` — the real hardware's 64x9 line buffer, emulated faithfully. Selection is per raster line AND order-dependent (first 16 matching win) |

**The Donkey Kong problem, stated so it is not rediscovered.** A column
crosses all 224 raster lines, so rendering it needs to know which sprites
won each line's arbitration. Recomputing that inside a column loop is 224
selections x 240 columns against today's 224 — 240x the work. It needs a
precomputed line->sprite table (224 lines x 16 entries, ~3.5KB). But
building that table is itself a whole-frame burst, which is precisely what
Phase 3 exists to remove. The workable shape is to build it INCREMENTALLY
DURING THE PREVIOUS FRAME, one line's arbitration per scanline slot, giving
landscape one frame of sprite latency. That is acceptable for a non-default
orientation but it is a design decision, not a mechanical port. Treat DKong
as its own piece of work, last.

Order: Pac-Man first (it has a harness and the smallest cached renderer),
confirm on hardware, then Ms. Pac-Man / Burger Time / Galaga, then DKong.

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

---

## 8. What is left, and how it gets verified

Two gaps remain, and they are independent — neither blocks the other.

| | state |
|---|---|
| **All orientations without red** | broken on the 5 tile+sprite games. Phase 3. |
| **Correct aspect ratio** | built and correct, OFF by default because DKong cannot afford it. Phase 2b's remaining lever. |

Space Invaders and Lunar Rescue are already clean in all four rotations
(bitmap VRAM, no compositing step, nothing to burst) — and they have ample
headroom, so they are the two games that could have the correction switched
on today.

**Which games actually need the correction**, worst first: Burger Time
(+33.3% too wide, and among the cheapest games), Invaders / Lunar Rescue /
DKong (+16.7%), Pac-Man / Ms. Pac-Man / Galaga (+3.7%, close to invisible).
Note the mismatch: the games that need it least are two of the three that
can least afford it. **Enabling it per-game rather than globally is a
legitimate outcome** if the emit rewrite does not land.

Cost is only known for DKong. Galaga is the other tight game (~14.5ms peak
after phase 1) and would likely also need the rewrite; the remaining five
have room.

### Verifying it

Most of this needs no hardware, which is the useful part:

- **Geometry, orientation, mirroring** — `tools/geom_test/` for the map
  arithmetic, and PPM byte-compares for the renderers. No board involved.
- **Red and frame budget** — the starve counter over serial, per game per
  rotation, via `-DTEST_ROTATION=n` and `-DTEST_STRETCH=1`. Objective.
- **Needs eyes, once per game** — that the picture is upright on the
  physically rotated monitor and the proportions look right.

**The practical bottleneck is the SD card.** Each game needs its own
`/rom/` contents, and the two 8080bw games discover ROMs by LISTING the
directory and sorting reverse-alphabetically — so a combined card would
break them (extra files become extra ROM banks). Hardware verification is
therefore serialised behind a card swap per game. Per-game subdirectories
would fix it; out of scope here, but it is the reason a full 7-game hardware
pass is a chore rather than a command.
