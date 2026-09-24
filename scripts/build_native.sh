#!/bin/sh
# Compile the native RTA/DP accelerator next to its C source.
set -eu
DIR="$(cd "$(dirname "$0")/../backend/app/native" && pwd)"
cc -O3 -std=c11 -Wall -Wextra -fPIC -shared \
    "$DIR/rta_core.c" -o "$DIR/librtacore.so"
echo "built $DIR/librtacore.so"
