#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-fileman-search.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
if [ ! -f "$src/userland/apps/fileman/search.cpp" ] ||
   [ ! -f "$src/userland/apps/fileman/search.h" ]; then
    printf 'FAIL - file manager search model is missing\n' >&2
    exit 1
fi
${HOSTCXX:-c++} -std=c++17 -D_XOPEN_SOURCE=700 -Wall -Wextra -Werror \
    -fno-exceptions -fno-rtti -fsanitize=address,undefined \
    -fno-sanitize-recover=all -I"$src/userland/apps/fileman" \
    "$src/tests/host/test_fileman_search.cpp" \
    "$src/userland/apps/fileman/search.cpp" -o "$work/test"
"$work/test" "$work"
