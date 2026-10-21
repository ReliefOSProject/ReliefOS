#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-$src/out/tmp}/reliefcapture-tests.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
${HOSTCXX:-c++} -std=c++17 -O2 -Wall -Wextra -Werror -nostdinc++ -fno-exceptions -fno-rtti \
    ${CAPTURE_TEST_FLAGS:-} "$src/tests/host/test_reliefcapture.cpp" \
    "$src/userland/apps/reliefcapture/model.cpp" -o "$work/verify"
"$work/verify"
${HOSTCXX:-c++} -std=c++17 -O2 -Wall -Wextra -Werror -nostdinc++ -fno-exceptions -fno-rtti \
    ${CAPTURE_TEST_FLAGS:-} "$src/tests/host/test_reliefcapture_png.cpp" \
    "$src/userland/apps/reliefcapture/model.cpp" "$src/userland/apps/reliefcapture/png_io.cpp" \
    $(pkg-config --cflags --libs libpng) -o "$work/png"
"$work/png" "$work"
