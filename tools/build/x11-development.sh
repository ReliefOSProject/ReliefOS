#!/bin/sh
# Build-only target headers and libraries; never overlay this root on the guest.
set -eu
[ "$#" = 6 ] || { printf 'usage: x11-development.sh SRC DEPS LOCK CACHE APK ROOT\n' >&2; exit 2; }
src=$(CDPATH= cd -- "$1" && pwd)
deps=$2
lock=$(CDPATH= cd -- "$(dirname "$3")" && pwd)/$(basename "$3")
cache=$(CDPATH= cd -- "$4" && pwd)
apk=$(CDPATH= cd -- "$(dirname "$5")" && pwd)/$(basename "$5")
mkdir -p "$(dirname "$6")"
root=$(CDPATH= cd -- "$(dirname "$6")" && pwd)/$(basename "$6")
mkdir -p "$root"
for id in x11-dev-motif x11-dev-libx11 x11-dev-libxt x11-dev-xorgproto x11-dev-libsm x11-dev-libice alpine-motif alpine-libx11 alpine-libxt; do
    url=$("$deps" --lock "$lock" --id "$id" --print url)
    digest=$("$deps" --lock "$lock" --id "$id" --print sha256)
    archive=$cache/${url##*/}
    [ -f "$archive" ] || { printf 'missing %s; run make fetch\n' "$archive" >&2; exit 1; }
    [ "$(sha256sum "$archive" | cut -d' ' -f1)" = "$digest" ] || { printf 'checksum mismatch: %s\n' "$archive" >&2; exit 1; }
    "$apk" --keys-dir "$src/system/rootfs/etc/apk/keys" verify "$archive"
    tar --ignore-zeros --warning=no-unknown-keyword -tzf "$archive" >"$root/members"
    : >"$root/selected"
    while IFS= read -r member; do
        case $member in */) continue;; esac
        case $member in
            usr/include/*|usr/lib/libXm.so*|usr/lib/libXt.so*|usr/lib/libX11.so*)
                case $member in *../*|*/../*|*'\'*) printf 'unsafe archive member\n' >&2; exit 1;; esac
                printf '%s\n' "$member" >>"$root/selected"
                ;;
        esac
    done <"$root/members"
    tar --ignore-zeros --warning=no-unknown-keyword -xzf "$archive" -C "$root" -T "$root/selected"
done
rm "$root/members" "$root/selected"
test -f "$root/usr/include/Xm/Xm.h"
test -f "$root/usr/include/X11/Intrinsic.h"
test -f "$root/usr/include/X11/X.h"
for name in Xm Xt X11; do test -f "$root/usr/lib/lib$name.so"; done
touch "$root/.complete"
