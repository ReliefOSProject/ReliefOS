#!/bin/sh
# Default browser configuration must never replace personal preferences.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
helper=$src/system/rootfs/usr/lib/reliefos/dillo-config-init
defaults=$src/system/rootfs/etc/skel/.dillo
test -x "$helper" || { echo 'FAIL - missing Dillo configuration initializer'; exit 1; }
work=$(mktemp -d "${TMPDIR:?}/dillo-config.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

mkdir "$work/new account"
"$helper" "$work/new account" "$defaults"
for name in dillorc style.css; do
    cmp "$defaults/$name" "$work/new account/.dillo/$name"
    test "$(stat -c %a "$work/new account/.dillo/$name")" = 600
done
printf 'font_sans_serif="Personal Font"\n' > "$work/new account/.dillo/dillorc"
printf 'body { color: blue; }\n' > "$work/new account/.dillo/style.css"
cp "$work/new account/.dillo/dillorc" "$work/personal.rc"
cp "$work/new account/.dillo/style.css" "$work/personal.css"
"$helper" "$work/new account" "$defaults"
cmp "$work/personal.rc" "$work/new account/.dillo/dillorc"
cmp "$work/personal.css" "$work/new account/.dillo/style.css"
: > "$work/new account/.dillo/style.css"
"$helper" "$work/new account" "$defaults"
test ! -s "$work/new account/.dillo/style.css"

rm "$work/new account/.dillo/style.css"
ln -s "$work/missing-personal-style" "$work/new account/.dillo/style.css"
"$helper" "$work/new account" "$defaults"
test -L "$work/new account/.dillo/style.css"
test ! -e "$work/missing-personal-style"
cmp "$work/personal.rc" "$work/new account/.dillo/dillorc"

mkdir "$work/linked account" "$work/external-config"
ln -s "$work/external-config" "$work/linked account/.dillo"
"$helper" "$work/linked account" "$defaults"
test -z "$(ls -A "$work/external-config")"

mkdir "$work/incomplete account" "$work/incomplete-defaults"
cp "$defaults/dillorc" "$work/incomplete-defaults/dillorc"
if "$helper" "$work/incomplete account" "$work/incomplete-defaults" 2>/dev/null; then
    echo 'FAIL - incomplete defaults were accepted'; exit 1
fi
test ! -e "$work/incomplete account/.dillo/dillorc"
printf '%s\n' 'Dillo defaults: first login, private modes, personal files and symlinks preserved'
