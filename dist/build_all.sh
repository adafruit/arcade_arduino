#!/bin/sh
# SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
#
# SPDX-License-Identifier: MIT

# Build all seven game sketches into dist/ as release-ready .uf2 files.
#
# TWO THINGS THIS DOES DELIBERATELY:
#
# 1. No --fqbn. Each sketch.yaml pins its own optimisation level (invaders
#    Optimize2, the rest Optimize3); passing one here would override every
#    game with a single setting. -Os in particular is not fast enough for
#    this video pipeline and shows up as red screens rather than as a
#    slower picture. See DEVNOTES.md.
#
# 2. A throwaway arduino-cli config pointing directories.user at this repo,
#    rather than relying on the caller having configured that globally (as
#    README.md's arduino-cli section describes). That makes the script work
#    from a fresh checkout, or from a secondary checkout, without touching
#    the caller's own arduino-cli setup.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
GAMES="invaders lrescue pacman mspacman btime dkong galaga"

CFG="$HERE/.arduino-cli.yaml"
{
    echo "board_manager:"
    echo "    additional_urls:"
    echo "        - https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json"
    echo "directories:"
    echo "    user: $ROOT"
} > "$CFG"

cd "$ROOT"
for g in $GAMES; do
    sk="${g}_fruitjam"
    printf '%-20s ' "$sk"
    if ! arduino-cli --config-file "$CFG" compile --output-dir "$HERE" "$sk" \
            > "$HERE/.$g.log" 2>&1; then
        echo "FAILED -- see $HERE/.$g.log"
        exit 1
    fi
    mv "$HERE/$sk.ino.uf2" "$HERE/$sk.uf2"
    # Everything else arduino-cli emits is intermediate. Removing it keeps
    # `gh release upload dist/*.uf2` picking up exactly the right set.
    rm -f "$HERE/$sk.ino."* "$HERE/.$g.log"
    printf 'ok   %s\n' "$(du -h "$HERE/$sk.uf2" | cut -f1 | tr -d ' ')"
done
rm -f "$CFG"

echo
echo "built into $HERE:"
ls -1 "$HERE"/*.uf2
