#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
export TMPDIR="$src/out/tmp"
mkdir -p "$TMPDIR"
work=$(mktemp -d "$TMPDIR/autostart.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
${HOSTCXX:-c++} -std=c++17 -D_GNU_SOURCE -Wall -Wextra -Werror \
    -DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_66 -DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_66 \
    -nostdinc++ -fno-exceptions -fno-rtti -fsanitize=address,undefined \
    -fno-sanitize-recover=all -g $(pkg-config --cflags gio-unix-2.0) \
    -I"$src/userland/apps" "$src/tests/host/test_autostart.cpp" \
    "$src/userland/apps/taskmgr/autostart.cpp" \
    "$src/userland/apps/taskmgr/autostart_runner.cpp" \
    $(pkg-config --libs gio-unix-2.0) -o "$work/test"
"$work/test" "$work"
