#!/bin/sh
set -eu
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
test_tmp_root=${TMPDIR:-/tmp}
mkdir -p "$test_tmp_root"
build_dir=$(mktemp -d "$test_tmp_root/reliefmath-model.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

${CXX:-c++} \
    -std=c++17 -Wall -Wextra -Werror \
    -fno-exceptions -fno-rtti \
    -fsanitize=address,undefined -fno-sanitize-recover=all -g \
    -DRM_TESTING \
    -iquote "$repo_root/userland/apps/reliefmath" \
    "$repo_root/tests/host/test_reliefmath_model.cpp" \
    "$repo_root/userland/apps/reliefmath/model.cpp" \
    "$repo_root/userland/apps/reliefmath/history.cpp" \
    -o "$build_dir/test_reliefmath_model"

"$build_dir/test_reliefmath_model"
echo "ReliefMath model suite passed"
