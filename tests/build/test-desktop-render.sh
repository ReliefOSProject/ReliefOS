#!/bin/sh
# Exercise the real userspace desktop compositor.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
cd "$src"
cc=${HOSTCC:-cc}
$cc -std=c11 -D_GNU_SOURCE -O1 -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -Iinclude -Ikernel/reliefnt/include/uapi -idirafter userland/runtime/include \
    tools/tests/desktop_render_test.c -o "$work/desktop"
for scenario in cursor assets alpha; do
    "$work/desktop" "$scenario"
done
