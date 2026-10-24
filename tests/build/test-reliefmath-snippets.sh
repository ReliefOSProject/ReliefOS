#!/bin/sh
set -eu

src=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/reliefmath-snippets.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

"${HOSTCXX:-c++}" -std=c++17 -Wall -Wextra -Werror -DRM_TESTING \
    -nostdinc++ \
    -fsanitize=address,undefined -fno-sanitize-recover=all -g \
    -fno-exceptions -fno-rtti -I"$src/userland/apps/reliefmath" \
    "$src/tests/host/test_reliefmath_snippets.cpp" \
    "$src/userland/apps/reliefmath/catalog.cpp" \
    "$src/userland/apps/reliefmath/snippet.cpp" \
    "$src/userland/apps/reliefmath/editor_core.cpp" \
    -Wl,--wrap=malloc,--wrap=realloc \
    -o "$work/test-reliefmath-snippets"

cd "$src"
"$work/test-reliefmath-snippets" "$@"
