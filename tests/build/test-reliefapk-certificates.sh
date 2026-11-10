#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
TMPDIR="$src/out/tmp"
export TMPDIR
mkdir -p "$TMPDIR"
. "$src/tests/lib/runtime-dependencies.sh"
work=$(mktemp -d "$TMPDIR/reliefapk-ca.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -m 700 "$work/trust"
${HOSTCXX:-c++} -std=c++17 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -nostdinc++ \
  -fsanitize=address,undefined -fno-sanitize-recover=all -g \
  -I"$src/userland/apps/reliefapk" -I"$runtime_deps/usr/include" \
  "$src/tools/tests/reliefapk_certificates_test.cpp" \
  "$src/userland/apps/reliefapk/certificate_io.cpp" \
  "$runtime_deps/usr/lib/libmbedx509.a" "$runtime_deps/usr/lib/libmbedcrypto.a" -pthread \
  -o "$work/recover"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  "$work/recover" "$work/trust" "$src/system/certs/cacert.pem"
