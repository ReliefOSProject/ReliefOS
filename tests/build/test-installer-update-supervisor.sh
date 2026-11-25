#!/bin/sh
set -eu
ulimit -c 0
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd -P)
work=$(mktemp -d "${TMPDIR:-$src/out/tmp}/installer-update-supervisor.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
${HOSTCC:-cc} -std=gnu11 -O1 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-sanitize-recover=all -g \
    "$src/tools/tests/installer_update_supervisor_test.c" \
    "$src/userland/apps/installer/installer_update_supervisor.c" -o "$work/test"
ASAN_OPTIONS=detect_leaks=1:handle_sigbus=0 UBSAN_OPTIONS=halt_on_error=1 "$work/test"
