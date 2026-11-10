#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-uefi.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
. "$src/tests/lib/runtime-dependencies.sh"
${HOSTCC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror -D_GNU_SOURCE \
  -fsanitize=address,undefined -fno-sanitize-recover=all -ffunction-sections -fdata-sections \
  -I"$work" -I"$src/userland/runtime/include" -I"$runtime_deps/usr/include" \
  "$src/tests/host/test_bootloader_uefi.c" "$src/userland/runtime/src/bootloader_uefi.c" \
  "$src/userland/runtime/src/bootloader.c" \
  "$src/userland/runtime/src/bootloader_publish.c" "$src/userland/runtime/src/kernel_bundle_io.c" \
  "$runtime_deps/usr/lib/libmbedcrypto.a" -pthread \
  -Wl,--gc-sections -o "$work/verify"
"$work/verify" "$work"
