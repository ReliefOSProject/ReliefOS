#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-$src/out/tmp}/reliefmathpp-tests.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
${HOSTCXX:-c++} -std=c++17 -O2 -Wall -Wextra -Werror -nostdinc++ -fno-exceptions -fno-rtti \
 "$src/tests/host/test_reliefmathpp.cpp" "$src/userland/apps/reliefmathpp/expression.cpp" \
 "$src/userland/apps/reliefmathpp/analysis.cpp" \
 "$src/userland/apps/reliefmathpp/model.cpp" "$src/userland/apps/reliefmathpp/mesh.cpp" \
 "$src/userland/apps/reliefmathpp/document.cpp" "$src/userland/apps/reliefmathpp/scene.cpp" -lm -o "$work/verify"
"$work/verify"
