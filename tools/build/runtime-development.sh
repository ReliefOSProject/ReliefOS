#!/bin/sh
# Build-only target headers and libraries; never overlay this root on the guest.
set -eu
[ "$#" = 6 ] || { printf 'usage: runtime-development.sh SRC DEPS LOCK CACHE APK ROOT\n' >&2; exit 2; }
src=$(CDPATH= cd -- "$1" && pwd)
deps=$2
lock=$(CDPATH= cd -- "$(dirname "$3")" && pwd)/$(basename "$3")
cache=$(CDPATH= cd -- "$4" && pwd)
apk=$(CDPATH= cd -- "$(dirname "$5")" && pwd)/$(basename "$5")
root_parent_input=$(dirname "$6")
root_name=$(basename "$6")
mkdir -p "$root_parent_input"
root_parent=$(CDPATH= cd -- "$root_parent_input" && pwd)
root=$root_parent/$root_name
if [ -L "$root" ] || { [ -e "$root" ] && [ ! -d "$root" ]; }; then
    printf 'development root is not a real directory: %s\n' "$root" >&2
    exit 1
fi
stage=$(mktemp -d "$root_parent/.${root_name}.new.XXXXXX")
backup=
cleanup() {
    status=$?
    if [ -n "$backup" ] && [ -e "$backup" ] &&
       [ ! -e "$root" ] && [ ! -L "$root" ]; then
        mv "$backup" "$root" || status=1
    fi
    if [ -n "$stage" ] && [ -e "$stage" ]; then
        rm -rf "$stage" || status=1
    fi
    if [ -n "$backup" ] && [ -e "$backup" ] &&
       { [ -e "$root" ] || [ -L "$root" ]; }; then
        rm -rf "$backup" || status=1
    fi
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' HUP INT TERM
# The -dev and -static archives are build inputs, not target-root overlays.
for id in alpine-zlib runtime-dev-zlib-dev runtime-dev-zlib-static \
    alpine-libpng runtime-dev-libpng-dev runtime-dev-libpng-static \
    alpine-sqlite-libs runtime-dev-sqlite-dev runtime-dev-sqlite-static \
    alpine-mbedtls3 runtime-dev-mbedtls3-dev runtime-dev-mbedtls3-static; do
    url=$("$deps" --lock "$lock" --id "$id" --print url)
    digest=$("$deps" --lock "$lock" --id "$id" --print sha256)
    archive=$cache/${url##*/}
    [ -f "$archive" ] || { printf 'missing %s; run make fetch\n' "$archive" >&2; exit 1; }
    [ "$(sha256sum "$archive" | cut -d' ' -f1)" = "$digest" ] || { printf 'checksum mismatch: %s\n' "$archive" >&2; exit 1; }
    "$apk" --keys-dir "$src/system/rootfs/etc/apk/keys" verify "$archive"
    tar --ignore-zeros --warning=no-unknown-keyword -tzf "$archive" >"$stage/members"
    : >"$stage/selected"
    while IFS= read -r member; do
        case $member in */) continue;; esac
        case $member in
            usr/include/*|usr/lib/libz.*|usr/lib/libpng*|usr/lib/libsqlite3.*|usr/lib/libmbed*|usr/lib/libeverest.a|usr/lib/libp256m.a)
                case $member in *../*|*/../*|*'\'*) printf 'unsafe archive member\n' >&2; exit 1;; esac
                printf '%s\n' "$member" >>"$stage/selected"
                ;;
        esac
    done <"$stage/members"
    tar --ignore-zeros --warning=no-unknown-keyword -xzf "$archive" -C "$stage" -T "$stage/selected"
    mkdir -p "$stage/usr/share/reliefos/sdk/apk"
    tar --ignore-zeros --warning=no-unknown-keyword -xOf "$archive" .PKGINFO >"$stage/usr/share/reliefos/sdk/apk/$id.PKGINFO"
done
rm "$stage/members" "$stage/selected"
for header in zlib.h zconf.h png.h pngconf.h pnglibconf.h sqlite3.h mbedtls/ssl.h mbedtls/mbedtls_config.h; do
    test -f "$stage/usr/include/$header"
done
for library in libz.so libz.so.1 libz.a libpng.so libpng16.so.16 libpng.a \
    libsqlite3.so libsqlite3.so.0 libsqlite3.a \
    libmbedtls.so libmbedtls.so.21 libmbedtls.a \
    libmbedx509.so libmbedx509.so.7 libmbedx509.a \
    libmbedcrypto.so libmbedcrypto.so.16 libmbedcrypto.a; do
    test -f "$stage/usr/lib/$library"
done
mkdir -p "$stage/usr/share/licenses"
for name in zlib libpng sqlite mbedtls3; do
    mkdir -p "$stage/usr/share/licenses/$name"
    cp "$src/resources/licenses/$name-LICENSE" "$stage/usr/share/licenses/$name/LICENSE"
done
touch "$stage/.complete"

if [ -e "$root" ]; then
    backup=$(mktemp -d "$root_parent/.${root_name}.old.XXXXXX")
    rmdir "$backup"
    mv "$root" "$backup"
fi
if ! mv "$stage" "$root"; then
    if [ -n "$backup" ]; then mv "$backup" "$root"; backup=; fi
    printf 'could not install staged development root: %s\n' "$root" >&2
    exit 1
fi
stage=
find "$root" -mindepth 1 ! -type d -printf '%P\n' | LC_ALL=C sort >"$root.files"
if [ -n "$backup" ]; then rm -rf "$backup"; backup=; fi
trap - EXIT HUP INT TERM
