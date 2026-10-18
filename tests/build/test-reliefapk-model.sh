#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
TMPDIR="$src/out/tmp"
export TMPDIR
mkdir -p "$TMPDIR"
work=$(mktemp -d "$TMPDIR/reliefapk-model.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
${HOSTCXX:-c++} -std=c++17 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
    -fno-exceptions -fno-rtti -nostdinc++ -fsanitize=address,undefined \
    -fno-sanitize-recover=all -g -I"$src/userland/apps/reliefapk" \
    "$src/tools/tests/reliefapk_model_test.cpp" \
    "$src/userland/apps/reliefapk/model.cpp" -o "$work/reliefapk-model"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/reliefapk-model"
