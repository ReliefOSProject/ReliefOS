#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-fileman-ops.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
for unit in archive filesystem; do
    if [ ! -f "$src/userland/apps/fileman/$unit.c" ]; then
        printf 'FAIL - file manager has no %s operation boundary\n' "$unit" >&2
        exit 1
    fi
done
${HOSTCC:-cc} -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
    -fsanitize=undefined -fno-sanitize-recover=all \
    -I"$src/userland/apps/fileman" "$src/tests/host/test_fileman_operations.c" \
    "$src/userland/apps/fileman/archive.c" "$src/userland/apps/fileman/filesystem.c" \
    "$src/userland/apps/fileman/boundary.c" -o "$work/test"
"$work/test" "$work"
