#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
export TMPDIR="$src/out/tmp"
mkdir -p "$TMPDIR"
work=$(mktemp -d "$TMPDIR/clipboard.XXXXXX")
server=
cleanup() {
    if [ -n "$server" ]; then kill "$server" 2>/dev/null || :; wait "$server" 2>/dev/null || :; fi
    rm -rf "$work"
}
trap cleanup EXIT HUP INT TERM
${HOSTCXX:-c++} -std=c++17 -D_GNU_SOURCE -Wall -Wextra -Werror \
    -fno-exceptions -fno-rtti -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$src/userland/apps" "$src/tests/host/test_clipboard.cpp" \
    "$src/userland/apps/clipboard/data.cpp" "$src/userland/apps/clipboard/history.cpp" \
    "$src/userland/apps/clipboard/selection.cpp" "$src/userland/apps/clipboard/manager.cpp" -lX11 -o "$work/test"
unset DISPLAY
if command -v Xvfb >/dev/null 2>&1; then
    Xvfb -displayfd 3 -screen 0 800x600x24 -nolisten tcp 3>"$work/display" >"$work/xvfb.log" 2>&1 &
    server=$!
    attempt=0
    while [ ! -s "$work/display" ]; do
        kill -0 "$server" 2>/dev/null || { cat "$work/xvfb.log"; exit 1; }
        attempt=$((attempt + 1))
        [ "$attempt" -lt 100 ] || exit 1
        sleep 0.05
    done
    DISPLAY=:$(cat "$work/display")
    export DISPLAY
fi
"$work/test" "$work/state"
