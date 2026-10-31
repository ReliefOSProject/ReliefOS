#!/bin/sh
# Build-only target headers and libraries; never overlay this root on the guest.
set -eu
[ "$#" = 6 ] || { printf 'usage: x11-development.sh SRC DEPS LOCK CACHE APK ROOT\n' >&2; exit 2; }
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
for id in x11-dev-fltk x11-dev-libx11 x11-dev-xorgproto alpine-libfltk alpine-libstdc++ alpine-libgcc alpine-libx11; do
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
            usr/include/*|usr/lib/libfltk.so*|usr/lib/libstdc++.so*|usr/lib/libgcc_s.so*|usr/lib/libX11.so*)
                case $member in *../*|*/../*|*'\'*) printf 'unsafe archive member\n' >&2; exit 1;; esac
                printf '%s\n' "$member" >>"$stage/selected"
                ;;
        esac
    done <"$stage/members"
    tar --ignore-zeros --warning=no-unknown-keyword -xzf "$archive" -C "$stage" -T "$stage/selected"
done
rm "$stage/members" "$stage/selected"
test -f "$stage/usr/include/FL/Fl.H"
test -f "$stage/usr/include/X11/Xlib.h"
test -f "$stage/usr/include/X11/X.h"
test -f "$stage/usr/lib/libfltk.so"
test -f "$stage/usr/lib/libfltk.so.1.3"
test -f "$stage/usr/lib/libstdc++.so.6"
test -f "$stage/usr/lib/libgcc_s.so.1"
test -f "$stage/usr/lib/libX11.so"
test -f "$stage/usr/lib/libX11.so.6"
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
if [ -n "$backup" ]; then rm -rf "$backup"; backup=; fi
trap - EXIT HUP INT TERM
