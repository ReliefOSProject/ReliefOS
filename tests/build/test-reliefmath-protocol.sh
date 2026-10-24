#!/bin/sh
set -eu

suite=${1:-all}
case "$suite" in
    all|protocol|process|worker) ;;
    *) echo "usage: $0 [all|protocol|process|worker]" >&2; exit 2 ;;
esac

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
host_cxx=${HOSTCXX:-${CXX:-c++}}
test_tmp_root=${TMPDIR:-/tmp}
mkdir -p "$test_tmp_root"
build_dir=$(mktemp -d "$test_tmp_root/reliefmath-protocol.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

common_flags="-std=c++17 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -fsanitize=address,undefined -fno-sanitize-recover=all -g"
include_flags="-iquote $repo_root/userland/apps/reliefmath"

if [ "$suite" = all ] || [ "$suite" = protocol ]; then
    # shellcheck disable=SC2086
    "$host_cxx" $common_flags $include_flags \
        "$repo_root/tests/host/test_reliefmath_protocol.cpp" \
        "$repo_root/userland/apps/reliefmath/protocol.cpp" \
        -o "$build_dir/test_reliefmath_protocol"
    "$build_dir/test_reliefmath_protocol"
fi

if [ "$suite" = all ] || [ "$suite" = process ]; then
    # shellcheck disable=SC2086
    "$host_cxx" $common_flags $include_flags \
        "$repo_root/tests/host/test_reliefmath_process.cpp" \
        "$repo_root/userland/apps/reliefmath/process.cpp" \
        "$repo_root/userland/apps/reliefmath/protocol.cpp" \
        "$repo_root/userland/apps/reliefmath/render_types.cpp" \
        "$repo_root/userland/apps/reliefmath/model.cpp" \
        -o "$build_dir/test_reliefmath_process"
    "$build_dir/test_reliefmath_process"
fi

if [ "$suite" = all ] || [ "$suite" = worker ]; then
    set -- $(pkg-config --cflags cairo freetype2)
    font_cflags="$*"
    set -- $(pkg-config --libs cairo freetype2)
    font_libs="$*"
    # shellcheck disable=SC2086
    "$host_cxx" $common_flags $font_cflags $include_flags \
        "$repo_root/userland/apps/reliefmath/worker/main.cpp" \
        "$repo_root/userland/apps/reliefmath/worker/boxes.cpp" \
        "$repo_root/userland/apps/reliefmath/worker/font.cpp" \
        "$repo_root/userland/apps/reliefmath/worker/output.cpp" \
        "$repo_root/userland/apps/reliefmath/worker/parser.cpp" \
        "$repo_root/userland/apps/reliefmath/worker/renderer.cpp" \
        "$repo_root/userland/apps/reliefmath/worker/scene.cpp" \
        "$repo_root/userland/apps/reliefmath/model.cpp" \
        "$repo_root/userland/apps/reliefmath/render_types.cpp" \
        "$repo_root/userland/apps/reliefmath/protocol.cpp" \
        $font_libs -o "$build_dir/reliefmath-render"
    "$build_dir/reliefmath-render" --render-worker \
        </dev/null >"$build_dir/worker.stdout" 2>"$build_dir/worker.stderr"
    test ! -s "$build_dir/worker.stdout"
fi

echo "ReliefMath $suite suite passed"
