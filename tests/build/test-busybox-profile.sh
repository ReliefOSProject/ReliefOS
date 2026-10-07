#!/bin/sh
set -eu

src=${1:-$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/source" "$work/build"
git -C "$src/third_party/busybox" archive HEAD | tar -xf - -C "$work/source"
for patch in "$src"/patches/busybox/*.patch; do
    [ -e "$patch" ] || continue
    patch -d "$work/source" -p1 < "$patch"
done
make -s -C "$work/source" O="$work/build" allnoconfig > "$work/config.log" 2>&1
awk 'FNR==NR {
    if ($0 ~ /^CONFIG_/) {split($0,a,"="); value[a[1]]=$0}
    else if ($0 ~ /^# CONFIG_.* is not set$/) value[$2]=$0
    next
}
{key=$1; sub(/=.*/,"",key); if($1=="#") key=$2;
 if(key in value) {print value[key]; delete value[key]} else print}
END {for(key in value) print value[key]}' \
    "$src/userland/busybox/leonos.config" "$work/build/.config" > "$work/build/.config.new"
mv "$work/build/.config.new" "$work/build/.config"
make -s -C "$work/source" O="$work/build" oldconfig </dev/null >> "$work/config.log" 2>&1
make -s -C "$work/source" O="$work/build" HOSTCC="${HOSTCC:-cc}" busybox.links >> "$work/config.log" 2>&1

# Check actual applet output after Kconfig dependencies and choices are resolved.
sh "$src/tools/build/busybox-check.sh" "$work/build/.config" "$work/build/busybox.links"
printf '%s\n' 'busybox generated profile ownership contract: PASS'
