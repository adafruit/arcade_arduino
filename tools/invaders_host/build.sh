#!/bin/sh
# SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
#
# SPDX-License-Identifier: MIT

# Build the Space Invaders host harness. See ../README.md.
#
# MACHINE_SRC / OUT can be overridden to build a SECOND binary from a
# different copy of ArcadeMachine_Invaders -- that is how an A/B digest
# comparison against a previous revision is done (see main.cpp's header):
#
#   MACHINE_SRC=/tmp/old/src OUT=/tmp/invaders_host_old ./build.sh
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
LIBS="$HERE/../../libraries"
OBJ="$HERE/build"
MACHINE_SRC="${MACHINE_SRC:-$LIBS/ArcadeMachine_Invaders/src}"
OUT="${OUT:-$HERE/invaders_host}"

INC="-I$HERE/../host_common/shim \
     -I$HERE/../host_common \
     -I$LIBS/ArcadeHAL/src \
     -I$LIBS/ArcadeCPU_i8080/src \
     -I$MACHINE_SRC"

mkdir -p "$OBJ"

# i8080.c is C (i8080.h carries its own extern "C" guards -- it is shared
# verbatim with the pure-C invaders_pico reference clone), so build it as C
# and link -- same split the Arduino build uses.
cc -O2 -g -std=c11 -Wall $INC -c "$LIBS/ArcadeCPU_i8080/src/i8080.c" -o "$OBJ/i8080.o"

c++ -O2 -g -std=c++17 -Wall -Wno-unused-parameter $INC \
    "$MACHINE_SRC"/*.cpp \
    "$HERE/../host_common/hal_host.cpp" \
    "$HERE/../host_common/host_ppm.cpp" \
    "$HERE/main.cpp" \
    "$OBJ/i8080.o" \
    -o "$OUT"

echo "built: $OUT"
