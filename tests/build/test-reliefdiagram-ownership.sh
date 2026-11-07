#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-$src/out/tmp}/reliefdiagram-ownership.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
${HOSTCC:-cc} -std=c11 -Wall -Wextra -Werror -I"$src" \
    "$src/tools/host/apk/reliefos-apk-own.c" "$src/tools/host/manifest/json.c" \
    "$src/tools/host/common/buffer.c" "$src/tools/host/common/io.c" -o "$work/own"
mkdir -p "$work/root/usr/lib/reliefos/apps/reliefdiagram" "$work/root/usr/bin" \
    "$work/root/usr/share/applications" "$work/root/usr/share/mime/packages"
printf 'fixture\n' > "$work/root/usr/lib/reliefos/apps/reliefdiagram/reliefdiagram.elf"
cp "$src/resources/build-art/app-icons/reliefdiagram.bmp" "$work/root/usr/lib/reliefos/apps/reliefdiagram/"
ln -s ../lib/reliefos/apps/reliefdiagram/reliefdiagram.elf "$work/root/usr/bin/reliefdiagram"
cp "$src/userland/apps/reliefdiagram/reliefdiagram.desktop" "$work/root/usr/share/applications/reliefos-reliefdiagram.desktop"
cp "$src/system/rootfs/usr/share/mime/packages/reliefdiagram.xml" "$work/root/usr/share/mime/packages/"
for size in 16 32 48 64 128 256; do
    path=usr/share/icons/hicolor/${size}x${size}/apps
    mkdir -p "$work/root/$path"
    cp "$src/resources/build-art/app-icons/hicolor/${size}x${size}/apps/reliefos-reliefdiagram.png" "$work/root/$path/"
done
"$work/own" --policy "$src/configs/apk-ownership.json" --root "$work/root" --output "$work/rows"
if ! awk -F '\t' '$2=="dir" {next} $1!="reliefos-reliefdiagram" {bad=1} {count++} END {exit bad || count!=11}' "$work/rows"; then
    cat "$work/rows"
    exit 1
fi
printf 'ReliefDiagram ownership: executable, launcher, MIME and artwork travel together\n'
