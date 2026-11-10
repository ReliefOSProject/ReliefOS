#!/bin/sh
# Run both target linkage modes after moving the actual SDK to a path with spaces.
set -eu
: "${SDK_ROOT:?fresh musl SDK required}"
: "${MUSL_LOADER:?host-compatible musl loader required}"
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
cp -a "$SDK_ROOT" "$work/relocated SDK"
for mode in dynamic static; do
    set --
    [ "$mode" != static ] || set -- -static
    "$work/relocated SDK/bin/reliefos-musl-cc" "$@" "$src/tools/tests/runtime_apk_probe.c" -lsqlite3 -o "$work/$mode"
    if [ "$mode" = dynamic ]; then
        "$MUSL_LOADER" --library-path "$work/relocated SDK/lib" "$work/$mode" "$work/$mode.db"
    else
        "$work/$mode" "$work/$mode.db"
    fi
done
printf 'ok - relocated SDK runs PNG/zlib/crypto/SQLite in both linkage modes\n'
