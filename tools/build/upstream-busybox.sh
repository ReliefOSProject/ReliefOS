#!/bin/sh
set -eu
MAKEFLAGS=${MAKEFLAGS-}
case ${MAKEFLAGS%% *} in *n*) exit 0 ;; esac
[ "$#" = 9 ] || { echo 'usage: upstream-busybox.sh SRC WORK OUTPUT MUSL HEADERS CC DEPS LOCK EPOCH' >&2; exit 2; }
src=$1 work=$2 output=$3 musl=$4 headers=$5 cc=$6 deps=$7 lock=$8 epoch=$9
revision=$("$deps" --lock "$lock" --id busybox --print commit)
[ "$(git -C "$src/third_party/busybox" rev-parse HEAD)" = "$revision" ] || exit 1
mkdir -p "$work/source" "$work/build" "$output"
git -C "$src/third_party/busybox" archive "$revision" > "$work/source.tar"
tar -xf "$work/source.tar" -C "$work/source"
# Command line overrides from the outer Make must not replace the configured CC.
SOURCE_DATE_EPOCH=$epoch KBUILD_BUILD_TIMESTAMP=$(date -u -d "@$epoch" '+%Y-%m-%d %H:%M:%S') KBUILD_BUILD_USER=reliefos KBUILD_BUILD_HOST=builder
export SOURCE_DATE_EPOCH KBUILD_BUILD_TIMESTAMP KBUILD_BUILD_USER KBUILD_BUILD_HOST
make -C "$work/source" O="$work/build" allnoconfig
awk 'FNR==NR { if ($0 ~ /^CONFIG_/){split($0,a,"=");if(a[1] !~ /^CONFIG_LEONOS_/ && a[1] != "CONFIG_EXTRA_LDLIBS") value[a[1]]=$0} else if($0 ~ /^# CONFIG_.* is not set$/) {value[$2]=$0} next } {key=$1; sub(/=.*/,"",key);if($1=="#")key=$2;if(key in value){print value[key];delete value[key]}else print} END {for(key in value)print value[key]}' "$src/userland/busybox/leonos.config" "$work/build/.config" > "$work/build/.config.new"
mv "$work/build/.config.new" "$work/build/.config"
make -C "$work/source" O="$work/build" oldconfig </dev/null
resource=$("$cc" -print-resource-dir)
CC=$cc; export CC
cflags="--target=x86_64-linux-musl ${UPSTREAM_CFLAGS:--O2} -std=gnu11 -ffreestanding -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -fno-stack-protector -fno-pic -fno-pie -ffunction-sections -fdata-sections -nostdinc -isystem $resource/include -I$musl/include -idirafter $headers"
ldflags="--target=x86_64-linux-musl -nostdlib -fuse-ld=lld -Wl,--gc-sections -Wl,--image-base=0x4000000 -L$musl/lib"
for flag in ${UPSTREAM_LDFLAGS-}; do ldflags="$ldflags -Wl,$flag"; done
startup="-Wl,$musl/lib/crt1.o -Wl,$musl/lib/crti.o -Wl,$musl/lib/mimalloc.o -Wl,$musl/lib/crtn.o"
make -C "$work/source" O="$work/build" "CC=$cc" ARCH=x86_64 "CFLAGS=$cflags" "LDFLAGS=$ldflags" LDLIBS=c "EXTRA_LDFLAGS=$startup" busybox_unstripped busybox.links
sh "$src/tools/build/busybox-check.sh" "$work/build/.config" "$work/build/busybox.links"
cp "$work/build/busybox_unstripped" "$output/busybox.elf.tmp"
chmod 755 "$output/busybox.elf.tmp"
mv "$output/busybox.elf.tmp" "$output/busybox.elf"
cp "$work/build/busybox.links" "$output/busybox.links.tmp"
mv "$output/busybox.links.tmp" "$output/busybox.links"
