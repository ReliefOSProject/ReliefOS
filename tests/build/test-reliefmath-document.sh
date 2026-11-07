#!/bin/sh
set -eu

src=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
mkdir -p "$src/out/tmp"
work=$(mktemp -d "$src/out/tmp/reliefmath-document.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

"${HOSTCXX:-c++}" \
    -std=c++17 -Wall -Wextra -Werror \
    -fno-exceptions -fno-rtti \
    -fsanitize=address,undefined -fno-sanitize-recover=all -g \
    -DRM_TESTING -iquote "$src/userland/apps/reliefmath" \
    "$src/tests/host/test_reliefmath_document.cpp" \
    "$src/userland/apps/reliefmath/model.cpp" \
    "$src/userland/apps/reliefmath/document.cpp" \
    "$src/userland/apps/reliefmath/file_io.cpp" \
    "$src/userland/apps/reliefmath/recovery.cpp" \
    -Wl,--wrap=fsync \
    -o "$work/test_reliefmath_document"

cd "$src"
"$work/test_reliefmath_document"
