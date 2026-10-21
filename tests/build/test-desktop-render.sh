#!/bin/sh
# Exercise the real desktop compositor and the ext4 DMA read boundary.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
cd "$src"
cc=${HOSTCC:-cc}
"$cc" -std=c11 -D_GNU_SOURCE -O1 -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -Iinclude -Ikernel/reliefnt/include/uapi -idirafter userland/runtime/include \
    tools/tests/desktop_render_test.c -o "$work/desktop"
for scenario in cursor assets alpha; do
    "$work/desktop" "$scenario"
done
"$cc" -std=c11 -O1 -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -DRELIEFOS_STORAGE_STANDALONE_TU -Iinclude -Ikernel/reliefnt/include/uapi \
    -Ikernel/reliefnt/include -Ikernel/reliefnt/include \
    tools/tests/ext4_dma_read_test.c -o "$work/dma"
"$work/dma"
"$work/dma" uncached
"$work/dma" no-memory
"$work/dma" high-memory
