#!/bin/sh
set -eu

src=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
tmp_root=${TMPDIR:-$src/out/tmp}
mkdir -p "$tmp_root"
work=$(mktemp -d "$tmp_root/reliefmath-dialogs.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

"${HOSTCXX:-c++}" -std=c++17 -Wall -Wextra -Werror -DRM_TESTING \
    -nostdinc++ -fsanitize=address,undefined -fno-sanitize-recover=all -g \
    -fno-exceptions -fno-rtti \
    -iquote "$src/userland/apps/reliefmath" \
    "$src/tests/host/test_reliefmath_dialogs.cpp" \
    "$src/userland/apps/reliefmath/library.cpp" \
    "$src/userland/apps/reliefmath/catalog.cpp" \
    "$src/userland/apps/reliefmath/editor_core.cpp" \
    "$src/userland/apps/reliefmath/snippet.cpp" \
    -o "$work/test-reliefmath-dialogs"

cd "$src"
"$work/test-reliefmath-dialogs" "$@"
