#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-fileman.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
if [ ! -f "$src/userland/apps/fileman/boundary.c" ]; then
    printf 'FAIL - file manager has no checked path and X11 handler boundary\n' >&2
    exit 1
fi
grep -Fq 'RELIEFOS_LAYOUT_RELIEFOS_APPS "/reliefexcel/reliefexcel.elf"' \
    "$src/userland/apps/fileman/main.cpp"
grep -Fq 'fileman_x11_app_for_path(path, user_association)' "$src/userland/apps/fileman/main.cpp"
${HOSTCC:-cc} -std=c11 -Wall -Wextra -Werror -fsanitize=undefined -fno-sanitize-recover=all \
    -I"$src/userland/apps/fileman" "$src/tests/host/test_fileman_boundary.c" \
    "$src/userland/apps/fileman/boundary.c" -o "$work/test"
"$work/test"
