#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-$src/out/tmp}/reliefdiagram-tests.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
${HOSTCXX:-c++} -std=c++17 -O2 -Wall -Wextra -Werror -nostdinc++ -fno-exceptions -fno-rtti \
  "$src/tests/host/test_reliefdiagram.cpp" "$src/userland/apps/reliefdiagram/model.cpp" \
  "$src/userland/apps/reliefdiagram/document.cpp" "$src/userland/apps/reliefdiagram/scene.cpp" -lm -o "$work/verify"
"$work/verify"
