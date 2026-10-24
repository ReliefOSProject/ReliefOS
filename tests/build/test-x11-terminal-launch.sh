#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
${HOSTCC:-cc} -std=c11 -O1 -g -ffunction-sections -fdata-sections \
    -Wl,--gc-sections -I"$src/include" -I"$src/kernel/reliefnt/include/uapi" \
    -idirafter "$src/userland/runtime/include" \
    "$src/tests/host/test_x11_terminal_launch.c" -o "$work/launch"
"$work/launch"
printf 'ok - terminal launch uses X11 st with literal argv and bounded argument count\n'
