#!/bin/sh
# The first real product must see native CONFIG values before its recipe runs.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd -P)
w=$(mktemp -d)
trap 'rm -rf "$w"' EXIT HUP INT TERM
make -s -C "$src" O="$w/out" generated/config/display.conf
[ -s "$w/out/.config" ]
[ -s "$w/out/include/config/auto.conf" ]
[ -s "$w/out/include/generated/autoconf.h" ]
grep -qx 'CONFIG_VMDK_DEFAULT_LANG=zh_CN.UTF-8' "$w/out/include/config/auto.conf"
grep -qx 'theme=metro' "$w/out/generated/config/display.conf"
grep -qx 'wallpaper.mode=fill' "$w/out/generated/config/display.conf"
printf 'first build: native configuration available to product rules passed\n'
