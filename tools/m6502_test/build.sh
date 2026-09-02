#!/bin/sh
# SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
#
# SPDX-License-Identifier: MIT

# Build the ArcadeCPU_M6502 conformance runner. See main.cpp's header for
# where to get the test binaries.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
LIBS="$HERE/../../libraries"
OBJ="$HERE/build"
OUT="$HERE/m6502_test"

INC="-I$LIBS/ArcadeCPU_M6502/src"

mkdir -p "$OBJ"

# m6502.c is C (m6502.h carries its own extern "C" guards), so build it as C
# and link -- same split the Arduino build and every other harness here use.
cc -O2 -g -std=c11 -Wall $INC -c "$LIBS/ArcadeCPU_M6502/src/m6502.c" -o "$OBJ/m6502.o"

c++ -O2 -g -std=c++17 -Wall -Wno-unused-parameter $INC \
    "$HERE/main.cpp" \
    "$OBJ/m6502.o" \
    -o "$OUT"

echo "built: $OUT"
