#!/bin/sh
# The catalog must be claimed per language: declaring usr/share/locale as a
# whole would silently retake ownership of the upstream catalogs in it.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)

grep -Fq 'LC_MESSAGES/$domain.mo' "$src/tools/build/rootfs-stage.sh"
grep -q 'reliefos-nls' "$src/tools/build/rootfs-stage.sh"
grep -q 'configs/nls/LINGUAS' "$src/tools/build/rootfs-stage.sh"
grep -q 'configs/nls/DOMAINS' "$src/tools/build/rootfs-stage.sh"

group=$(sed -n '/"reliefos-nls"/,/^[[:space:]]*}[[:space:]]*$/p' \
    "$src/configs/apk-ownership.json")
[ -n "$group" ] || { printf 'FAIL - reliefos-nls group missing\n'; exit 1; }
echo "$group" | grep -q '"destination": "reliefos"'
echo "$group" | grep -q '"usr/share/locale/zh_CN"'
echo "$group" | grep -q '"usr/share/locale/ja_JP"'
echo "$group" | grep -q '"usr/share/musl/locales/ja_JP.UTF-8"'
if echo "$group" | grep -q '"usr/share/locale"'; then
    printf 'FAIL - group claims the whole locale tree\n'
    exit 1
fi

printf 'nls stage: ownership granularity and plan wiring passed\n'
