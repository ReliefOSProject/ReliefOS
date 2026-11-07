#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-$src/out/tmp}/reliefword-tests.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
for suite in model document layout; do
    ${HOSTCXX:-c++} -std=c++17 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
        -fno-exceptions -fno-rtti -fsanitize=address,undefined -fno-sanitize-recover=all -g \
        -I"$src/userland/apps/reliefword" \
        "$src/tests/host/test_reliefword_$suite.cpp" \
        "$src/userland/apps/reliefword/model.cpp" \
        "$src/userland/apps/reliefword/document.cpp" \
        "$src/userland/apps/reliefword/layout.cpp" -lm -o "$work/$suite"
    if [ "$suite" = layout ]; then
        ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/$suite"
    elif [ "$suite" = document ]; then
        TMPDIR="$work" ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/$suite"
    else
        ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/$suite" "$work"
    fi
done
printf 'ReliefWord host suites passed\n'
