#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-$src/out/tmp}/reliefcapture-x11.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
${HOSTCXX:-c++} -std=c++17 -O2 -Wall -Wextra -Werror -nostdinc++ -fno-exceptions -fno-rtti \
    "$src/tests/host/test_reliefcapture_x11.cpp" "$src/userland/apps/reliefcapture/model.cpp" \
    "$src/userland/apps/reliefcapture/capture.cpp" "$src/userland/apps/reliefcapture/job.cpp" \
    "$src/userland/apps/reliefcapture/keyboard.cpp" \
    "$src/userland/apps/reliefcapture/png_io.cpp" $(pkg-config --cflags --libs x11 libpng) -o "$work/verify"
xvfb-run -a -s '-screen 0 640x480x24 -nolisten tcp' "$work/verify"
