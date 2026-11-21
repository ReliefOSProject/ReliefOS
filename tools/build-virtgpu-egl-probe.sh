#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Non-production integration probe; no downloads or host GL libraries are used.
set -eu
if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
    echo "usage: $0 RUNTIME_ROOT OUTPUT [GRAPHICS_HEADER_ROOT]" >&2
    exit 2
fi
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
runtime=$(CDPATH= cd -- "$1" && pwd)
output=$2
headers=${3:-/usr/include}
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT HUP INT TERM
# Copy only graphics API declarations so musl never consumes host glibc headers.
cp -R "$headers/EGL" "$headers/GLES2" "$headers/GLES3" "$headers/KHR" "$headers/gbm.h" "$temporary/"
mkdir -p "$(dirname "$output")"
"${CC:-musl-gcc}" -std=c11 -O2 -Wall -Wextra -Werror -I"$temporary" \
    -I"$src/kernel/reliefnt/include/uapi" \
    "$src/tools/tests/virtgpu_egl_probe.c" \
    -Wl,-rpath-link,"$runtime/usr/lib" -Wl,-rpath-link,"$runtime/lib" \
    "$runtime/usr/lib/libEGL.so.1" "$runtime/usr/lib/libGLESv2.so.2" \
    "$runtime/usr/lib/libgbm.so.1" -o "$output"
