#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-$src/out/tmp}/reliefplayer-ownership.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
${HOSTCC:-cc} -std=c11 -Wall -Wextra -Werror -I"$src" \
    "$src/tools/host/apk/reliefos-apk-own.c" "$src/tools/host/manifest/json.c" \
    "$src/tools/host/common/buffer.c" "$src/tools/host/common/io.c" -o "$work/own"
mkdir -p "$work/root/usr/lib/reliefos/apps/reliefplayer" "$work/root/usr/bin" "$work/root/usr/share/applications"
printf 'fixture\n' > "$work/root/usr/lib/reliefos/apps/reliefplayer/reliefplayer.elf"
cp "$src/resources/build-art/app-icons/reliefplayer.bmp" "$work/root/usr/lib/reliefos/apps/reliefplayer/"
ln -s ../lib/reliefos/apps/reliefplayer/reliefplayer.elf "$work/root/usr/bin/reliefplayer"
cp "$src/userland/apps/reliefplayer/reliefplayer.desktop" "$work/root/usr/share/applications/reliefos-reliefplayer.desktop"
for size in 16 32 48 64 128 256; do
    path=usr/share/icons/hicolor/${size}x${size}/apps
    mkdir -p "$work/root/$path"
    cp "$src/resources/build-art/app-icons/hicolor/${size}x${size}/apps/reliefos-reliefplayer.png" "$work/root/$path/"
done
"$work/own" --policy "$src/configs/apk-ownership.json" --root "$work/root" --output "$work/rows"
if ! awk -F '\t' '$2=="dir" {next} $1!="reliefos-reliefplayer" {bad=1} {count++} END {exit bad || count!=10}' "$work/rows"; then
    cat "$work/rows"; exit 1
fi
printf 'ReliefPlayer ownership: executable, launcher, desktop entry and artwork travel together\n'
mkdir -p "$work/root/usr/share/licenses/ffmpeg"
printf 'LGPL fixture\n' > "$work/root/usr/share/licenses/ffmpeg/COPYING.LGPLv2.1"
for lib in avformat avcodec avutil swresample swscale; do
    printf 'library fixture\n' > "$work/root/usr/lib/lib$lib-reliefplayer.so.62.1.0"
    ln -s "lib$lib-reliefplayer.so.62.1.0" "$work/root/usr/lib/lib$lib-reliefplayer.so.62"
done
"$work/own" --policy "$src/configs/apk-ownership.json" --root "$work/root" --output "$work/rows-codecs"
awk -F '\t' '$2=="dir" {next} $4 ~ /-reliefplayer.so|licenses\/ffmpeg/ {if($1!="reliefos-media-codecs")exit 1;count++} END {if(count!=11)exit 1}' "$work/rows-codecs"
printf 'ReliefPlayer codec ownership: private SONAMEs, versioned links and LGPL notice grouped separately\n'
