#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
base=${TMPDIR:-$src/out/tmp}
mkdir -p "$base"
work=$(mktemp -d "$base/reliefmath-workflows.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
suite=${1:-jobs}
core="$src/userland/apps/reliefmath"
test_ldflags=
case $suite in
jobs) sources="$core/jobs.cpp $core/model.cpp $core/document.cpp $core/file_io.cpp $core/recovery.cpp";;
export) sources="$core/export.cpp $core/jobs.cpp $core/process.cpp $core/protocol.cpp $core/render_types.cpp $core/model.cpp $core/document.cpp $core/file_io.cpp $core/recovery.cpp"; test_ldflags="-Wl,--wrap=waitpid";;
ui) sources="$core/ui_layout.cpp";;
*) exit 2;;
esac
${HOSTCXX:-c++} -std=c++17 -Wall -Wextra -Werror -nostdinc++ -fno-exceptions -fno-rtti \
    -fsanitize=address,undefined -fno-sanitize-recover=all -pthread -g -iquote "$core" \
    "$src/tests/host/test_reliefmath_$suite.cpp" $sources $test_ldflags -o "$work/test"
"$work/test" "$work"
printf 'ReliefMath %s suite passed\n' "$suite"
