#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
export TMPDIR="$src/out/tmp"
mkdir -p "$TMPDIR"
work=$(mktemp -d "$TMPDIR/reliefsettings.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
${HOSTCXX:-c++} -std=c++17 -Wall -Wextra -Werror -nostdinc++ -fno-exceptions -fno-rtti \
    -fsanitize=address,undefined -fno-sanitize-recover=all -g \
    -I"$src/userland/apps/reliefsettings" \
    "$src/tools/tests/reliefsettings_test.cpp" \
    "$src/userland/apps/reliefsettings/model.cpp" \
    "$src/userland/apps/reliefsettings/config.cpp" -Wl,--wrap=fsync -o "$work/test"
"$work/test" "$work/preferences"
