#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-fileman.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
if [ ! -f "$src/userland/apps/fileman/boundary.c" ]; then
    printf 'FAIL - file manager has no checked path and X11 handler boundary\n' >&2
    exit 1
fi
${HOSTCC:-cc} -std=c11 -Wall -Wextra -Werror -fsanitize=undefined -fno-sanitize-recover=all \
    -I"$src/userland/apps/fileman" "$src/tests/host/test_fileman_boundary.c" \
    "$src/userland/apps/fileman/boundary.c" -o "$work/test"
"$work/test"
