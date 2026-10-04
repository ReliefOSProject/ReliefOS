#!/bin/sh
# Build the pinned, unmodified LGPL Nuked OPL3 C library in a private O tree.
set -eu
MAKEFLAGS=${MAKEFLAGS-}
case ${MAKEFLAGS%% *} in *n*) exit 0;; esac
[ "$#" = 12 ] || { echo 'usage: opl3.sh PACKAGE SRC DEPS LOCK CACHE WORK STAGE MUSL AUTH PAM CC TARGET' >&2; exit 2; }
pkg=$1 deps=$3 lock=$4 cache=$5 work=$6 stage=$7 musl=$8 cc=${11} target=${12}
[ "$pkg" = nuked-opl3 ] || exit 2
get() { "$deps" --lock "$lock" --id "$pkg" --print "$1"; }
url=$(get url) digest=$(get sha256) directory=$(get directory)
archive=$cache/${url##*/}
[ -f "$archive" ] || { echo 'missing nuked-opl3 archive; run make fetch' >&2; exit 1; }
[ "$(sha256sum "$archive" | cut -d' ' -f1)" = "$digest" ] || { echo 'nuked-opl3: archive digest mismatch' >&2; exit 1; }
mkdir -p "$work" "$(dirname "$stage")"
tar -tf "$archive" > "$work/members"
if LC_ALL=C grep -E '(^/|(^|/)\.\.(/|$))' "$work/members"; then exit 1; fi
rm -rf "$work/source" "$work/build"
mkdir -p "$work/source" "$work/build"
tar -xf "$archive" -C "$work/source" --no-same-owner
source=$work/source/$directory
tmp=$(mktemp -d "$stage.new.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/usr/lib" "$tmp/usr/include" "$tmp/usr/share/licenses/$pkg"
resource=$("$cc" -print-resource-dir)
"$cc" --target="$target" --sysroot="$musl" --gcc-toolchain=/nonexistent \
 -nostdinc -isystem "$musl/include" -isystem "$resource/include" \
 -O2 -std=gnu17 -mno-avx -mno-avx2 -fPIC -c "$source/opl3.c" -o "$work/build/opl3.o"
"$cc" --target="$target" --sysroot="$musl" --gcc-toolchain=/nonexistent \
 -fuse-ld=lld --rtlib=compiler-rt --unwindlib=none -L"$musl/lib" \
 -shared -Wl,-soname,libopl3.so.1,-z,relro,-z,now \
 "$work/build/opl3.o" -o "$tmp/usr/lib/libopl3.so.1"
ln -s libopl3.so.1 "$tmp/usr/lib/libopl3.so"
cp "$source/opl3.h" "$tmp/usr/include/"
license=$(get license_in_source)
cp "$source/$license" "$tmp/usr/share/licenses/$pkg/"
printf 'Upstream: %s\nCommit: %s\nSHA-256: %s\n' "$url" "$(get commit)" "$digest" > "$tmp/usr/share/licenses/$pkg/SOURCE"
printf '%s\n' "$pkg $digest $target" > "$tmp/.complete"
if [ -d "$stage.previous" ] && [ ! -e "$stage" ]; then mv "$stage.previous" "$stage"; fi
rm -rf "$stage.previous"
if [ -e "$stage" ]; then mv "$stage" "$stage.previous"; fi
mv "$tmp" "$stage"
rm -rf "$stage.previous"
(cd "$stage" && find . \( -type f -o -type l \) ! -name .complete -print | LC_ALL=C sort) > "$work/installed-files"
