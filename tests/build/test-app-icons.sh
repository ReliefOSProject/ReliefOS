#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-$src/out/tmp}/app-icons.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
python3 "$src/tools/tests/test_app_icons.py"
${HOSTCXX:-c++} -std=c++17 -Wall -Wextra -Werror -fno-exceptions -fno-rtti \
    -fsanitize=address,undefined -fno-sanitize-recover=all -I"$src/userland/apps" \
    "$src/tests/host/test_app_icon_pixels.cpp" -o "$work/pixels"
"$work/pixels" "$src/resources/build-art/app-icons/fileman.bmp" "$work/invalid.bmp"
if command -v desktop-file-validate >/dev/null 2>&1; then
    desktop-file-validate "$src"/userland/apps/*/*.desktop
fi
