#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
mkdir -p "${TMPDIR:-/tmp}"
work=$(mktemp -d "${TMPDIR:-/tmp}/calc-modes.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -nostdinc++ -fno-exceptions -fno-rtti \
    -fsanitize=address,undefined -fno-sanitize-recover=all -g \
    -iquote "$src/userland/apps/calc" "$src/tests/host/test_calc_modes.cpp" \
    "$src/userland/apps/calc/model.cpp" "$src/userland/apps/calc/programmer.cpp" \
    "$src/userland/apps/calc/units.cpp" "$src/userland/apps/calc/utilities.cpp" \
    -lm -o "$work/calc-modes"
"$work/calc-modes"
