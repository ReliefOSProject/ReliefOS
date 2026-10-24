#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
${HOSTCC:-cc} -std=c11 -Wall -Wextra -Werror -O1 -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    "$src/tests/host/test_askpass_password.c" -o "$work/password"
"$work/password"
printf 'ok - askpass bounds UTF-8 bytes, rejects multiline input and handles short/interrupted writes\n'
