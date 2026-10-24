#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: sh tests/build/test-reliefmath-layout.sh MATH_FONT TEXT_FONT" >&2
    exit 2
fi
math_font=$1
text_font=$2
host_cxx=$(printenv HOSTCXX 2>/dev/null || true)
if [ -z "$host_cxx" ]; then
    host_cxx=c++
fi

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
work_root=$(printenv TMPDIR 2>/dev/null || true)
if [ -z "$work_root" ]; then
    work_root=/tmp
fi
mkdir -p "$work_root"
build_dir=$(mktemp -d "$work_root/reliefmath-layout.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

set -- $(pkg-config --cflags freetype2 cairo)
font_cflags="$*"
set -- $(pkg-config --libs freetype2 cairo)
font_libs="$*"

# shellcheck disable=SC2086
"$host_cxx" \
    -std=c++17 -Wall -Wextra -Werror \
    -nostdinc++ \
    -fno-exceptions -fno-rtti \
    -fsanitize=address,undefined -fno-sanitize-recover=all -g \
    $font_cflags \
    -iquote "$repo_root/userland/apps/reliefmath" \
    "$repo_root/tests/host/test_reliefmath_layout.cpp" \
    "$repo_root/userland/apps/reliefmath/catalog.cpp" \
    "$repo_root/userland/apps/reliefmath/snippet.cpp" \
    "$repo_root/userland/apps/reliefmath/worker/parser.cpp" \
    "$repo_root/userland/apps/reliefmath/worker/boxes.cpp" \
    "$repo_root/userland/apps/reliefmath/worker/scene.cpp" \
    "$repo_root/userland/apps/reliefmath/worker/font.cpp" \
    $font_libs \
    -o "$build_dir/test-reliefmath-layout"

cd "$repo_root"
"$build_dir/test-reliefmath-layout" "$math_font" "$text_font"
