#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
mode=${1:---host}
work=$src/out/tmp/reliefmath-backend
mkdir -p "$work"
case $mode in
--host)
    mkdir -p "$work/fonts" "$work/images"
    ln -sf "$src/resources/reliefmath/STIXTwoMath-Regular.otf" "$work/fonts/STIXTwoMath-Regular.otf"
    ln -sf "$src/system/fonts/simsun.ttc" "$work/fonts/simsun.ttc"
    sources=
    for file in "$src"/userland/apps/reliefmath/worker/*.cpp; do
        case $file in */main.cpp) continue;; esac
        sources="$sources $file"
    done
    ${HOSTCXX:-c++} -std=c++17 -Wall -Wextra -Werror -fno-exceptions -fno-rtti \
        -fsanitize=address,undefined -fno-sanitize-recover=all -g \
        -iquote "$src/userland/apps/reliefmath" $(pkg-config --cflags cairo freetype2) \
        "$src/tools/tests/reliefmath_backend_probe.cpp" \
        "$src/userland/apps/reliefmath/model.cpp" "$src/userland/apps/reliefmath/render_types.cpp" \
        $sources $(pkg-config --libs cairo freetype2) -o "$work/probe"
    "$work/probe" "$work/fonts" "$work/images"
    ;;
--target)
    target=$src/out/xorg-vmware-debug/tests/reliefmath-backend-probe.elf
    test -f "$target"
    readelf -h "$target" >"$work/target-header.txt"
    readelf -l -d "$target" >"$work/target-dynamic.txt"
    grep -q 'Advanced Micro Devices X86-64' "$work/target-header.txt"
    grep -q '/lib/ld-musl-x86_64.so.1' "$work/target-dynamic.txt"
    grep -q 'libfreetype.so.6' "$work/target-dynamic.txt"
    grep -q 'libcairo.so.2' "$work/target-dynamic.txt"
    ;;
--check-evidence)
    test -s "$src/docs/RELIEFMATH_BACKEND.md"
    test -s "$src/resources/licenses/reliefmath/STIX-OFL.txt"
    test "$(sha256sum "$src/resources/reliefmath/STIXTwoMath-Regular.otf" | cut -d' ' -f1)" = 3a5f3f26f40d5698b3c62dd085d48d6663696a3f80825aab8b553d5097518e8c
    test -s "$work/images/probe-4.svg"
    test -s "$work/images/probe-4.png"
    test -s "$work/target-header.txt"
    ;;
*) echo 'unsupported backend test mode' >&2; exit 2;;
esac
