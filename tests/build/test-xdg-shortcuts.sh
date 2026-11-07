#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-xdg-shortcuts.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
${HOSTCC:-cc} -std=c11 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$src/include" \
    -idirafter "$src/kernel/reliefnt/include/uapi" \
    -idirafter "$src/userland/runtime/include" \
    "$src/tests/host/test_xdg_shortcuts.c" "$src/userland/runtime/src/desktop_entry.c" \
    "$src/userland/runtime/src/xdg.c" \
    -o "$work/test"
mkdir "$work/home"
"$work/test" "$work/home"
if command -v gio >/dev/null 2>&1; then
    desktop=$(cat "$work/home/application.desktop")
    export XDG_SHORTCUT_MARKER="$work/home/launched"
    gio launch "$desktop"
    n=0
    while [ ! -f "$XDG_SHORTCUT_MARKER" ] && [ "$n" -lt 100 ]; do
        sleep .01; n=$((n + 1))
    done
    [ "$(cat "$XDG_SHORTCUT_MARKER")" = launched ]
    printf 'ok - real GLib GIO launches the quoted UTF-8 Desktop Entry\n'
fi
