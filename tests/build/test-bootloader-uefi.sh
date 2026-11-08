#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-uefi.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
printf '#define MBEDTLS_SHA256_C\n' > "$work/qa_mbedtls.h"
${HOSTCC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror -D_GNU_SOURCE \
  -fsanitize=address,undefined -fno-sanitize-recover=all -ffunction-sections -fdata-sections \
  -DMBEDTLS_CONFIG_FILE='"qa_mbedtls.h"' -I"$work" -I"$src/userland/runtime/include" -I"$src/third_party/mbedtls/include" \
  "$src/tests/host/test_bootloader_uefi.c" "$src/userland/runtime/src/bootloader_uefi.c" \
  "$src/userland/runtime/src/bootloader.c" \
  "$src/userland/runtime/src/bootloader_publish.c" "$src/userland/runtime/src/kernel_bundle_io.c" \
  "$src/third_party/mbedtls/library/sha256.c" "$src/third_party/mbedtls/library/platform_util.c" \
  -Wl,--gc-sections -o "$work/verify"
"$work/verify" "$work"
