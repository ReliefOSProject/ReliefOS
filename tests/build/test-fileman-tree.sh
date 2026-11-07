#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-fileman-tree.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
${HOSTCC:-cc} -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Werror \
    -ffunction-sections -fdata-sections -fsanitize=undefined -fno-sanitize-recover=all \
    -I"$src/userland/apps/fileman" -I"$src/userland/runtime/include" -I"$src/include" \
    -I"$src/kernel/reliefnt/include/uapi" \
    "$src/tests/host/test_fileman_tree.c" "$src/userland/apps/fileman/model.c" \
    "$src/userland/apps/fileman/state.c" "$src/userland/apps/fileman/util.c" \
    "$src/userland/apps/fileman/boundary.c" \
    "$src/userland/runtime/src/fs_directory.c" -Wl,--gc-sections -o "$work/test"
"$work/test" "$work"
