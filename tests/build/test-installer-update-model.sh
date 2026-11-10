#!/bin/sh
set -eu
ulimit -c 0
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd -P)
work=$(mktemp -d "${TMPDIR:-$src/out/tmp}/installer-update-model.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir "$work/root"
${HOSTCC:-cc} -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-sanitize-recover=all -g \
    -I"$src/userland/apps/installer" \
    "$src/tools/tests/installer_update_model_test.c" \
    "$src/userland/apps/installer/installer_update_model.c" \
    "$src/userland/apps/installer/installer_update_journal.c" -o "$work/test"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/test" "$work/root"
