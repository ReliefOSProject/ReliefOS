#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-$src/out/tmp}/reliefplayer-mp4.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
ffmpeg -v error -nostdin -f lavfi -i testsrc2=size=320x180:rate=12:duration=2 \
  -itsoffset 0.3 -f lavfi -i sine=frequency=440:sample_rate=44100:duration=1.7 \
  -c:v libx264 -threads 1 -g 24 -bf 2 -pix_fmt yuv420p -c:a aac -ac 2 "$work/video.mp4"
ffmpeg -v error -nostdin -i "$work/video.mp4" -an -c:v copy -movflags +faststart "$work/silent.mp4"
ffmpeg -v error -nostdin -i "$work/video.mp4" -c:v copy -c:a alac "$work/unsupported.mp4"
ffmpeg -v error -nostdin -display_rotation 90 -i "$work/silent.mp4" -c copy "$work/rotated.mp4"
ffmpeg -v error -nostdin -i "$work/silent.mp4" -c:v libx265 -x265-params pools=none:frame-threads=1:log-level=error -t 0.5 "$work/hevc.mp4"
ffmpeg -v error -nostdin -i "$work/silent.mp4" -c:v mpeg4 -t 0.5 "$work/mpeg4.mp4"
head -c 100 "$work/video.mp4" > "$work/truncated.mp4"
${HOSTCXX:-c++} -std=c++17 -O2 -Wall -Wextra -Werror -nostdinc++ -fno-exceptions -fno-rtti \
  ${PLAYER_TEST_FLAGS:-} "$src/tests/host/test_reliefplayer_mp4.cpp" \
  "$src/userland/apps/reliefplayer/decoder.cpp" "$src/userland/apps/reliefplayer/media.cpp" \
  "$src/userland/apps/reliefplayer/pcm.cpp" "$src/userland/apps/reliefplayer/session.cpp" \
  "$src/userland/apps/reliefplayer/worker.cpp" -I"$src/third_party/minimp3" \
  $(pkg-config --cflags --libs libavformat libavcodec libavutil libswresample libswscale) -lm -o "$work/verify"
if [ -n "${PLAYER_TARGET_BUILD:-}" ]; then
  codecs=$PLAYER_TARGET_BUILD/upstream/ffmpeg/root
  musl=$PLAYER_TARGET_BUILD/sysroot/musl
  ${TARGETCXX:-clang++} --target=x86_64-linux-musl --sysroot="$musl" --gcc-toolchain=/nonexistent \
    -fuse-ld=lld --rtlib=compiler-rt --unwindlib=none -std=c++17 -O2 \
    -nostdinc++ -nostdlib++ -fno-exceptions -fno-rtti -D_GNU_SOURCE -I"$src/third_party/minimp3" -I"$codecs/usr/include" \
    -I"$PLAYER_TARGET_BUILD/kernel-export/include" \
    "$src/tests/host/test_reliefplayer_mp4.cpp" "$src/userland/apps/reliefplayer/decoder.cpp" \
    "$src/userland/apps/reliefplayer/media.cpp" "$src/userland/apps/reliefplayer/pcm.cpp" \
    "$src/userland/apps/reliefplayer/session.cpp" "$src/userland/apps/reliefplayer/worker.cpp" \
    -L"$musl/lib" -L"$codecs/usr/lib" -lavformat-reliefplayer -lavcodec-reliefplayer \
    -lswresample-reliefplayer -lswscale-reliefplayer -lavutil-reliefplayer -lm -o "$work/verify-target"
  "$musl/lib/libc.so" --library-path "$codecs/usr/lib:$musl/lib" "$work/verify-target" \
    "$work/video.mp4" "$work/silent.mp4" "$work/truncated.mp4" "$work/unsupported.mp4" \
    "$work/rotated.mp4" "$work/hevc.mp4" "$work/mpeg4.mp4"
fi
"$work/verify" "$work/video.mp4" "$work/silent.mp4" "$work/truncated.mp4" "$work/unsupported.mp4" \
  "$work/rotated.mp4" "$work/hevc.mp4" "$work/mpeg4.mp4"
