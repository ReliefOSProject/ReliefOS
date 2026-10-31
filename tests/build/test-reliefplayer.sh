#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-$src/out/tmp}/reliefplayer-tests.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
${HOSTCXX:-c++} -std=c++17 -O2 -Wall -Wextra -Werror -nostdinc++ -fno-exceptions -fno-rtti \
    ${PLAYER_TEST_FLAGS:-} "$src/tests/host/test_reliefplayer.cpp" \
    "$src/userland/apps/reliefplayer/decoder.cpp" "$src/userland/apps/reliefplayer/pcm.cpp" \
    -I"$src/third_party/minimp3" -lm -o "$work/verify"
"$work/verify" "$work" "${PLAYER_MP3_FIXTURE:-$src/test/test.mp3}"
