#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-fileman-watch.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
${HOSTCC:-cc} -std=c11 -Wall -Wextra -Werror -fsanitize=undefined \
    -fno-sanitize-recover=all -I"$src/userland/apps/fileman" \
    -I"$src/kernel/reliefnt/include/uapi" \
    "$src/tests/host/test_fileman_watch.c" "$src/userland/apps/fileman/watch.c" \
    "$src/userland/apps/fileman/selection.c" -o "$work/test"
"$work/test" "$work"
