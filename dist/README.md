<!--
SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
SPDX-License-Identifier: MIT
-->

# `dist/` — built firmware, for testing and for release assets

**Not checked in.** `.uf2` files are covered by `.gitignore`; only this
README is tracked. The binaries here are build output, and on GitHub they
belong as **release assets**, not as repository contents.

## Flashing one

Hold **BOOT** while connecting USB (or hold BOOT and tap **RESET**), then
copy the `.uf2` onto the `RP2350` drive that appears.

Each binary still needs its game's microSD card — no ROM data is baked in.
See the per-game README for the `/rom/` and `/samples/` layout it expects.

## Rebuilding the whole set

```sh
./dist/build_all.sh
```

Each sketch pins its own optimisation level in its `sketch.yaml`
(`default_fqbn`), and the script deliberately does NOT pass `--fqbn`, so each
game builds the way it was measured. **This matters**: Space Invaders is
pinned to `Optimize2` and the rest to `Optimize3`, and `-Os` is not fast
enough for this pipeline — it produces red screens rather than a clean
slower picture. See DEVNOTES.md.

The builds are plain defaults: no `TEST_ROTATION`, `TEST_STRETCH` or
`TEST_AUTOSTART`. Those exist for measurement and would ship a game stuck in
one rotation or playing itself.

## Uploading to a release

```sh
gh release create vX.Y.Z dist/*.uf2 --title "..." --notes "..."
# or, for an existing release:
gh release upload vX.Y.Z dist/*.uf2
```
