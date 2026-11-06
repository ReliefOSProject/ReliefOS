#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-xdg-homes.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
input=$work/input
mkdir -p "$input/etc/skel" "$input/etc/reliefos"
cp "$src/system/rootfs/etc/sudoers" "$input/etc/sudoers"
printf '# shell configuration\n' > "$input/etc/skel/.bashrc"
for mode in disk live; do
    root=$work/$mode
    sh "$src/tools/build/standalone-root.sh" "$src" "$input" "$root" "$mode" C
    for home in root home/test; do
        [ "$(stat -c '%a' "$root/$home")" = 700 ]
        cmp "$input/etc/skel/.bashrc" "$root/$home/.bashrc"
        for retired in desktop documents downloads; do
            [ ! -e "$root/$home/$retired" ]
        done
    done
done
[ ! -e "$input/home/test" ]
printf 'ok - disk/live account homes defer special directories to XDG initialization\n'
