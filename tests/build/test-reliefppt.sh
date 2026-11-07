#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-$src/out/tmp}/reliefppt-tests.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
for suite in model document layout; do
    ${HOSTCXX:-c++} -std=c++17 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
        -fno-exceptions -fno-rtti -fsanitize=address,undefined -fno-sanitize-recover=all -g \
        -I"$src/userland/apps/reliefppt" \
        "$src/tests/host/test_reliefppt_$suite.cpp" \
        "$src/userland/apps/reliefppt/model.cpp" \
        "$src/userland/apps/reliefppt/document.cpp" \
        "$src/userland/apps/reliefppt/geometry.cpp" -lm -o "$work/$suite"
    TMPDIR="$work" ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/$suite"
done
${HOSTCXX:-c++} -std=c++17 -Wall -Wextra -Werror -fno-exceptions -fno-rtti \
    -DRP_TEST_TRACK_ALLOCATIONS -I"$src/userland/apps/reliefppt" \
    "$src/tests/host/test_reliefppt_model.cpp" "$src/userland/apps/reliefppt/model.cpp" \
    -Wl,--wrap=malloc -lm -o "$work/model-allocations"
"$work/model-allocations"
${HOSTCXX:-c++} -std=c++17 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
    -Wno-unused-function -fno-exceptions -fno-rtti -DRPPT_TEST_RENAMEAT2_WRAP \
    -fsanitize=address,undefined -fno-sanitize-recover=all -g \
    -I"$src/userland/apps/reliefppt" \
    "$src/tests/host/test_reliefppt_document.cpp" "$src/userland/apps/reliefppt/model.cpp" \
    "$src/userland/apps/reliefppt/document.cpp" -lm -Wl,--wrap=syscall -o "$work/rename-fallback"
TMPDIR="$work" ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/rename-fallback"
printf 'ReliefPPT sanitizer suites, allocation failures and rename fallback regressions passed\n'
