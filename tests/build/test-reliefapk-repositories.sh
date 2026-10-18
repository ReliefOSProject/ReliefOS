#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
mkdir -p "${TMPDIR:-$src/out/tmp}"
work=$(mktemp -d "${TMPDIR:-$src/out/tmp}/reliefapk-repositories.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
${HOSTCXX:-c++} -std=c++17 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
    -fno-exceptions -fno-rtti -fsanitize=address,undefined -g \
    -I"$src/userland/apps/reliefapk" "$src/tools/tests/reliefapk_repositories_test.cpp" \
    "$src/userland/apps/reliefapk/repositories.cpp" -o "$work/test"
TMPDIR="$work" ASAN_OPTIONS=detect_leaks=1 "$work/test"
