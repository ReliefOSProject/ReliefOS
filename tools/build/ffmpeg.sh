#!/bin/sh
# Minimal LGPL MP4 decoding libraries; all input is pinned and fetched explicitly.
set -eu
MAKEFLAGS=${MAKEFLAGS-}
case ${MAKEFLAGS%% *} in *n*) exit 0;; esac
[ "$#" = 12 ] || exit 2
pkg=$1 deps=$3 lock=$4 cache=$5 work=$6 stage=$7 musl=$8 cc=${11} target=${12}
[ "$pkg" = ffmpeg ] || exit 2
get() { "$deps" --lock "$lock" --id "$pkg" --print "$1"; }
url=$(get url) digest=$(get sha256) directory=$(get directory)
archive=$cache/${url##*/}
[ -f "$archive" ] || { echo 'missing FFmpeg archive; run make fetch' >&2; exit 1; }
[ "$(sha256sum "$archive" | cut -d' ' -f1)" = "$digest" ] || { echo 'FFmpeg archive digest mismatch' >&2; exit 1; }
mkdir -p "$work" "$(dirname "$stage")"
tar -tf "$archive" > "$work/members"
if LC_ALL=C grep -E '(^/|(^|/)\.\.(/|$))' "$work/members"; then exit 1; fi
rm -rf "$work/source" "$work/build"
mkdir -p "$work/source" "$work/build"
tar -xf "$archive" -C "$work/source" --no-same-owner
source=$work/source/$directory
tmp=$(mktemp -d "$stage.new.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
cd "$work/build"
resource=$("$cc" -print-resource-dir)
sh "$source/configure" --prefix=/usr --libdir=/usr/lib --build-suffix=-reliefplayer --arch=x86_64 --target-os=linux \
  --enable-cross-compile --cc="$cc" --ld="$cc" --ar=llvm-ar --ranlib=llvm-ranlib \
  --strip=llvm-strip --sysroot="$musl" \
  --extra-cflags="--target=$target --gcc-toolchain=/nonexistent -nostdinc -isystem $musl/include -isystem $resource/include -O2 -mno-avx -mno-avx2" \
  --extra-ldflags="--target=$target --gcc-toolchain=/nonexistent -fuse-ld=lld --rtlib=compiler-rt --unwindlib=none -L$musl/lib -Wl,-z,relro,-z,now" \
  --disable-everything --disable-autodetect --disable-network --disable-programs \
  --disable-doc --disable-debug --disable-static --enable-shared --enable-pic \
  --disable-x86asm --disable-avdevice --disable-avfilter \
  --enable-avformat --enable-avcodec --enable-avutil --enable-swscale --enable-swresample \
  --enable-demuxer=mov --enable-decoder=h264,hevc,mpeg4,aac,mp3 \
  --enable-parser=h264,hevc,mpeg4video,aac,mpegaudio
make
make DESTDIR="$tmp" install
mkdir -p "$tmp/usr/share/licenses/ffmpeg"
cp "$source/COPYING.LGPLv2.1" "$source/LICENSE.md" "$tmp/usr/share/licenses/ffmpeg/"
printf 'Upstream: %s\nCommit: %s\nSHA-256: %s\nConfiguration: %s\n' \
  "$url" "$(get commit)" "$digest" "$(cat ffbuild/config.mak | sed -n 's/^FFMPEG_CONFIGURATION=//p')" \
  > "$tmp/usr/share/licenses/ffmpeg/SOURCE"
printf '%s\n' "$pkg $digest $target" > "$tmp/.complete"
if [ -d "$stage.previous" ] && [ ! -e "$stage" ]; then mv "$stage.previous" "$stage"; fi
rm -rf "$stage.previous"
if [ -e "$stage" ]; then mv "$stage" "$stage.previous"; fi
mv "$tmp" "$stage"
rm -rf "$stage.previous"
(cd "$stage" && find . \( -type f -o -type l \) ! -name .complete -print | LC_ALL=C sort) > "$work/installed-files"
