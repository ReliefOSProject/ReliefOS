#!/bin/sh
set -eu
ulimit -c 0
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd -P)
work=$(mktemp -d "${TMPDIR:-$src/out/tmp}/installer-update-engine.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/target" "$work/media/usr/share/reliefos/apk/repository"
printf 'fixture package archive\n' > "$work/media/usr/share/reliefos/apk/repository/fixture.apk"
${HOSTCC:-cc} -std=c11 -D_GNU_SOURCE -O1 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-sanitize-recover=all -g \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -I"$src/userland/runtime/include" -I"$src/include" -idirafter "$src/kernel/reliefnt/include/uapi" \
    "$src/tools/tests/installer_update_engine_test.c" \
    "$src/userland/apps/installer/installer_update_supervisor.c" \
    "$src/userland/apps/installer/installer_update_progress.c" -o "$work/test"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/test" "$work"
