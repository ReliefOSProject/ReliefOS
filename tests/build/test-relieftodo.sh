#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/relieftodo-tests.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
${HOSTCXX:-c++} -std=c++17 -O2 -Wall -Wextra -Werror -nostdinc++ \
    -fno-exceptions -fno-rtti \
    "$src/tools/tests/relieftodo_host_verify.cpp" \
    "$src/userland/apps/relieftodo/model.cpp" \
    "$src/userland/apps/relieftodo/document.cpp" \
    "$src/userland/apps/relieftodo/file_io.cpp" -o "$work/verify"
"$work/verify"
printf '%s\n' 'ok - ReliefTodo model, history, document and file transactions'
