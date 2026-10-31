#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
tmpbase=${TMPDIR:-$src/out/tmp}
mkdir -p "$tmpbase"
work=$(mktemp -d "$tmpbase/reliefapk-process.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
${HOSTCXX:-c++} -std=c++17 -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L \
    -Wall -Wextra -Werror -fno-exceptions -fno-rtti \
    -fsanitize=address,undefined -fno-sanitize-recover=all -g \
    -I"$src/userland/apps/reliefapk" \
    "$src/tools/tests/reliefapk_process_test.cpp" -o "$work/process-test"
TMPDIR="$work" ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
    "$work/process-test" "$work"
