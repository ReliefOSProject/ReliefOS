#!/bin/sh
set -eu
ulimit -c 0
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd -P)
work=$(mktemp -d "${TMPDIR:-$src/out/tmp}/installer-update-target.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
${HOSTCC:-cc} -std=c11 -D_GNU_SOURCE -O1 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-sanitize-recover=all -g \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -I"$src/userland/runtime/include" \
    "$src/tools/tests/installer_update_target_test.c" -o "$work/test"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/test"
