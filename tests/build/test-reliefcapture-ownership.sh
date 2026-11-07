#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd -P)
tmp_base=$TMPDIR
if [ -z "$tmp_base" ]; then tmp_base=$src/out/tmp; fi
mkdir -p "$tmp_base"
work=$(mktemp -d "$tmp_base/reliefcapture-ownership.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
hostcc=$(printenv HOSTCC || true)
if [ -z "$hostcc" ]; then hostcc=cc; fi
$hostcc -std=c11 -Wall -Wextra -Werror -I"$src" \
    "$src/tools/host/apk/reliefos-apk-own.c" "$src/tools/host/manifest/json.c" \
    "$src/tools/host/common/buffer.c" "$src/tools/host/common/io.c" -o "$work/own"
mkdir -p "$work/root/usr/lib/reliefos/apps/reliefcapture" \
    "$work/root/usr/bin" "$work/root/usr/share/applications"
printf 'fixture\n' > "$work/root/usr/lib/reliefos/apps/reliefcapture/reliefcapture.elf"
printf 'bitmap fixture\n' > "$work/root/usr/lib/reliefos/apps/reliefcapture/reliefcapture.bmp"
ln -s ../lib/reliefos/apps/reliefcapture/reliefcapture.elf "$work/root/usr/bin/reliefcapture"
printf 'desktop fixture\n' > "$work/root/usr/share/applications/reliefos-reliefcapture.desktop"
for size in 16 32 48 64 128 256; do
    path=usr/share/icons/hicolor/$size'x'$size/apps
    mkdir -p "$work/root/$path"
    printf 'PNG fixture %s\n' "$size" > "$work/root/$path/reliefos-reliefcapture.png"
done
"$work/own" --policy "$src/configs/apk-ownership.json" \
    --root "$work/root" --output "$work/rows"
if ! awk -F '\t' '$2=="dir" {next} $1!="reliefos-reliefcapture" {bad=1} {count++} END {exit bad || count!=10}' "$work/rows"; then
    cat "$work/rows"
    exit 1
fi
printf 'ReliefCapture ownership: executable, launcher and all artwork belong to its standalone APK\n'
