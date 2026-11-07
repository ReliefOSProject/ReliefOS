#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/reliefmath-recovery-flow.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
${HOSTCXX:-c++} -std=c++17 -Wall -Wextra -Werror -nostdinc++ \
    -fno-exceptions -fno-rtti -fsanitize=address,undefined \
    -fno-sanitize-recover=all -g -iquote "$src/userland/apps/reliefmath" \
    "$src/tests/host/test_reliefmath_recovery_flow.cpp" -o "$work/test"
"$work/test"
printf 'ReliefMath recovery flow suite passed\n'
