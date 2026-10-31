#!/bin/sh
# Verified, build-only C graphics headers; never overlay this tree on the guest.
set -eu
[ "$#" = 6 ] || exit 2
src=$(CDPATH= cd -- "$1" && pwd)
deps=$2 lock=$3 cache=$4 apk=$5 root=$6
mkdir -p "$(dirname "$root")"
stage=$(mktemp -d "$root.new.XXXXXX")
backup=
cleanup() {
    status=$?
    if [ -n "$backup" ] && [ -e "$backup" ] && [ ! -e "$root" ]; then mv "$backup" "$root" || status=1; fi
    if [ -n "$stage" ] && [ -e "$stage" ]; then rm -rf "$stage" || status=1; fi
    if [ -n "$backup" ] && [ -e "$backup" ] && [ -e "$root" ]; then rm -rf "$backup" || status=1; fi
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' HUP INT TERM
for id in reliefmath-dev-cairo reliefmath-dev-freetype alpine-cairo alpine-freetype; do
    url=$("$deps" --lock "$lock" --id "$id" --print url)
    digest=$("$deps" --lock "$lock" --id "$id" --print sha256)
    archive=$cache/${url##*/}
    test -f "$archive" || { echo "missing $id: run make fetch" >&2; exit 1; }
    test "$(sha256sum "$archive" | cut -d' ' -f1)" = "$digest"
    "$apk" --keys-dir "$src/system/rootfs/etc/apk/keys" verify "$archive"
    tar --ignore-zeros --warning=no-unknown-keyword -tzf "$archive" >"$stage/members"
    : >"$stage/selected"
    while IFS= read -r member; do
        case $member in
            */) continue;;
            usr/include/cairo/*|usr/include/freetype2/*|usr/lib/libcairo.so*|usr/lib/libfreetype.so*)
                case $member in *../*|*'\'*) exit 1;; esac
                printf '%s\n' "$member" >>"$stage/selected";;
        esac
    done <"$stage/members"
    tar --ignore-zeros --warning=no-unknown-keyword -xzf "$archive" -C "$stage" -T "$stage/selected"
done
rm "$stage/members" "$stage/selected"
test -f "$stage/usr/include/cairo/cairo.h"
test -f "$stage/usr/include/freetype2/ft2build.h"
test -f "$stage/usr/lib/libcairo.so"
test -f "$stage/usr/lib/libfreetype.so"
# Publish a complete tree; remove stale members when inputs change.
test ! -L "$root"
if [ -e "$root" ] && [ ! -d "$root" ]; then echo 'invalid development root' >&2; exit 1; fi
test -f "$stage/usr/lib/libcairo.so.2"
test -f "$stage/usr/lib/libfreetype.so.6"
(cd "$stage" && find usr \( -type f -o -type l \) -print | LC_ALL=C sort) >"$stage/.members"
touch "$stage/.complete"
if [ -e "$root" ]; then
    backup=$(mktemp -d "$root.old.XXXXXX")
    rmdir "$backup"
    mv "$root" "$backup"
fi
mv "$stage" "$root"
stage=
if [ -n "$backup" ]; then rm -rf "$backup"; backup=; fi
trap - EXIT HUP INT TERM
