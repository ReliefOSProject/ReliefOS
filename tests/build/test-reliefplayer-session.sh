#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-$src/out/tmp}/reliefplayer-session.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
${HOSTCXX:-c++} -std=c++17 -O2 -Wall -Wextra -Werror -nostdinc++ -fno-exceptions -fno-rtti \
    "$src/tests/host/test_reliefplayer_session.cpp" \
    "$src/userland/apps/reliefplayer/decoder.cpp" "$src/userland/apps/reliefplayer/pcm.cpp" \
    "$src/userland/apps/reliefplayer/worker.cpp" "$src/userland/apps/reliefplayer/session.cpp" \
    -I"$src/third_party/minimp3" -lm -o "$work/verify"
"$work/verify" "$src/test/test.mp3"
${HOSTCXX:-c++} -std=c++17 -O2 -Wall -Wextra -Werror -nostdinc++ -fno-exceptions -fno-rtti \
    "$src/tests/host/test_reliefplayer_drain.cpp" \
    "$src/userland/apps/reliefplayer/decoder.cpp" "$src/userland/apps/reliefplayer/pcm.cpp" \
    "$src/userland/apps/reliefplayer/worker.cpp" "$src/userland/apps/reliefplayer/session.cpp" \
    -I"$src/third_party/minimp3" -lm -Wl,--wrap=open,--wrap=close,--wrap=write,--wrap=ioctl -o "$work/drain"
"$work/drain" "$work/drain.wav"
