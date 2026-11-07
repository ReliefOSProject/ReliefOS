#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/reliefexcel-tests.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
for unit in model edit formula document main; do
    if [ ! -f "$src/userland/apps/reliefexcel/$unit.cpp" ]; then
        printf 'FAIL - ReliefExcel %s implementation is missing\n' "$unit" >&2
        exit 1
    fi
done
for suite in storage model formula document; do
    ${HOSTCXX:-c++} -std=c++17 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
        -fno-exceptions -fno-rtti -fsanitize=address,undefined -fno-sanitize-recover=all -g \
        -I"$src/userland/apps/reliefexcel" \
        "$src/tests/host/test_reliefexcel_$suite.cpp" \
        "$src/userland/apps/reliefexcel/model.cpp" \
        "$src/userland/apps/reliefexcel/edit.cpp" \
        "$src/userland/apps/reliefexcel/formula.cpp" \
        "$src/userland/apps/reliefexcel/document.cpp" -lm -o "$work/$suite"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/$suite" "$work"
done
printf 'ReliefExcel host suites passed\n'
