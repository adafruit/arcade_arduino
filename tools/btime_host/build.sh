#!/bin/sh
# SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
#
# SPDX-License-Identifier: MIT

# Build the Burger Time host harness. See ../README.md.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
LIBS="$HERE/../../libraries"
OBJ="$HERE/build"
OUT="$HERE/btime_host"

INC="-I$HERE/../host_common/shim \
     -I$LIBS/ArcadeHAL/src \
     -I$LIBS/ArcadeCPU_M6502/src \
     -I$LIBS/ArcadeMachine_BTime/src"

mkdir -p "$OBJ"

# m6502.c is C (m6502.h carries its own extern "C" guards), so build it as C
# and link -- same split the Arduino build uses. Both of this machine's CPUs
# are instances of this one core.
cc -O2 -g -std=c11 -Wall $INC -c "$LIBS/ArcadeCPU_M6502/src/m6502.c" -o "$OBJ/m6502.o"

c++ -O2 -g -std=c++17 -Wall -Wno-unused-parameter $INC \
    "$LIBS/ArcadeMachine_BTime/src"/*.cpp \
    "$HERE/../host_common/hal_host.cpp" \
    "$HERE/main.cpp" \
    "$OBJ/m6502.o" \
    -o "$OUT"

echo "built: $OUT"
