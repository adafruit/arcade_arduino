#!/bin/sh
# SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
#
# SPDX-License-Identifier: MIT

# Build the Donkey Kong host harness. See ../README.md.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
LIBS="$HERE/../../libraries"
OBJ="$HERE/build"
OUT="$HERE/dkong_host"

INC="-I$HERE/../host_common/shim \
     -I$HERE/../host_common \
     -I$LIBS/ArcadeHAL/src \
     -I$LIBS/ArcadeCPU_Z80/src \
     -I$LIBS/ArcadeCPU_MCS48/src \
     -I$LIBS/ArcadeMachine_DKong/src"

mkdir -p "$OBJ"

# z80.c is C (z80.h carries its own extern "C" guards), so build it as C
# and link -- same split the Arduino build uses.
cc -O2 -g -std=c11 -Wall $INC -c "$LIBS/ArcadeCPU_Z80/src/z80.c" -o "$OBJ/z80.o"
# The MCS-48 core (Donkey Kong's 8035 sound CPU) is C as well.
cc -O2 -g -std=c11 -Wall $INC -c "$LIBS/ArcadeCPU_MCS48/src/mcs48.c" -o "$OBJ/mcs48.o"

c++ -O2 -g -std=c++17 -Wall -Wno-unused-parameter $INC \
    "$LIBS/ArcadeMachine_DKong/src"/*.cpp \
    "$HERE/../host_common/hal_host.cpp" \
    "$HERE/../host_common/host_ppm.cpp" \
    "$HERE/main.cpp" \
    "$OBJ/z80.o" \
    "$OBJ/mcs48.o" \
    -o "$OUT"

echo "built: $OUT"
