#!/bin/sh
# Native libraries for sanitized Linux host tests; never target/SDK inputs.
set -eu
. "$(CDPATH= cd -- "$(dirname "$0")" && pwd)/upstream-make-environment.sh"
reliefos_upstream_make_environment
[ "$#" = 7 ] || { echo 'usage: host-runtime-development.sh DEPS LOCK CACHE TARGET_DEV WORK ROOT CC' >&2; exit 2; }
deps=$1 lock=$2 cache=$3 target=$4 work=$5 root=$6 cc=$7
mkdir -p "$work" "$(dirname "$root")"
for id in host-test-mbedtls3 host-test-sqlite3; do
    url=$("$deps" --lock "$lock" --id "$id" --print url)
    digest=$("$deps" --lock "$lock" --id "$id" --print sha256)
    archive=$cache/${url##*/}
    [ -f "$archive" ] || { echo "missing $id archive; run make fetch" >&2; exit 1; }
    [ "$(sha256sum "$archive" | cut -d' ' -f1)" = "$digest" ] || { echo "checksum mismatch: $id" >&2; exit 1; }
    directory=$("$deps" --lock "$lock" --id "$id" --print directory)
    rm -rf "$work/$directory"
    tar -xf "$archive" -C "$work"
done
stage=$(mktemp -d "$root.new.XXXXXX")
trap 'rm -rf "$stage"' EXIT HUP INT TERM
mkdir -p "$stage/usr/include" "$stage/usr/lib"
cp -a "$target/usr/include/." "$stage/usr/include/"
# Preserve the official package configuration while compiling for the host libc.
make -C "$work/mbedtls-3.6.7/library" CC="$cc" AR=ar \
    CFLAGS="-O2 -fPIC -DMBEDTLS_CONFIG_FILE='\"$stage/usr/include/mbedtls/mbedtls_config.h\"'" \
    libmbedcrypto.a libmbedx509.a libmbedtls.a
cp "$work/mbedtls-3.6.7/library/"libmbed*.a "$stage/usr/lib/"
$cc -O2 -fPIC -pthread -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_RTREE \
    -DSQLITE_ENABLE_COLUMN_METADATA -c "$work/sqlite-autoconf-3530400/sqlite3.c" -o "$work/sqlite3.o"
ar rcsD "$stage/usr/lib/libsqlite3.a" "$work/sqlite3.o"
cp "$work/sqlite-autoconf-3530400/sqlite3.h" "$stage/usr/include/"
touch "$stage/.complete"
rm -rf "$root.previous"
[ ! -e "$root" ] || mv "$root" "$root.previous"
mv "$stage" "$root"
rm -rf "$root.previous"
find "$root" -mindepth 1 ! -type d -printf '%P\n' | LC_ALL=C sort >"$root.files"
