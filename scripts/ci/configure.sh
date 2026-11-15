#!/usr/bin/env bash
set -euo pipefail

size=${INSTALLER_ROOT_SIZE_MIB:-400}
if ! [[ "$size" =~ ^[0-9]{1,4}$ ]] || (( 10#$size < 32 || 10#$size > 4096 )); then
  echo 'installer_root_size_mib must be an integer from 32 to 4096' >&2
  exit 2
fi

git submodule status --recursive
test -f third_party/musl/configure
test -f third_party/busybox/Makefile
make fetch
# The parent fetch delegates to ReliefNT and verifies both restored caches.
make defconfig
config=out/x86_64/release/.config
sed -i '/^CONFIG_INSTALLER_ROOT_SIZE_MIB=/d;/^# CONFIG_INSTALLER_ROOT_SIZE_MIB is not set$/d' "$config"
printf 'CONFIG_INSTALLER_ROOT_SIZE_MIB=%d\n' "$((10#$size))" >> "$config"
make olddefconfig
if make CONFIG_NOT_A_REAL_OPTION=y kernel; then
  echo 'unknown CONFIG override unexpectedly succeeded' >&2
  exit 1
fi
