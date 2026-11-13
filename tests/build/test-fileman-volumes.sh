#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-$src/out/tmp}/fileman-volumes.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
${HOSTCXX:-c++} -std=c++17 -D_GNU_SOURCE -Wall -Wextra -Werror \
    -fno-exceptions -fno-rtti \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -idirafter "$src/userland/runtime/include" -I"$src/userland/apps/fileman" \
    "$src/tests/host/test_fileman_volumes.cpp" \
    "$src/userland/apps/fileman/volumes.cpp" -o "$work/test"
"$work/test"
