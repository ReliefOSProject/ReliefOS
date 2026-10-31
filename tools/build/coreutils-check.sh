#!/bin/sh
# Validate the complete GNU command namespace before publishing its stage.
set -eu
[ "$#" = 3 ] || { echo 'usage: coreutils-check.sh ROOT PROGRAMS UPSTREAM_PROGRAMS' >&2; exit 2; }
root=$1 programs=$2 upstream=$3
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
awk '
$0 != "[" && $0 !~ /^[A-Za-z0-9][A-Za-z0-9._+-]*$/ {bad=1}
seen[$0]++ {bad=1}
END {if (NR == 0 || bad) exit 1}' "$programs" || {
    echo 'coreutils: unsafe or duplicate command inventory' >&2; exit 1;
}
LC_ALL=C sort "$programs" > "$work/expected"
awk '$0=="ginstall" {print "install"; next} $0!="libstdbuf.so" {print}' "$upstream" |
    LC_ALL=C sort > "$work/upstream"
cmp -s "$work/expected" "$work/upstream" || {
    echo 'coreutils: command inventory differs from the locked upstream source' >&2; exit 1;
}
[ -f "$root/usr/bin/coreutils" ] && [ ! -L "$root/usr/bin/coreutils" ] &&
    [ -x "$root/usr/bin/coreutils" ] || {
    echo 'coreutils: missing executable GNU dispatcher' >&2; exit 1;
}
while IFS= read -r name; do
    [ "$name" != coreutils ] || continue
    [ -L "$root/usr/bin/$name" ] &&
        [ "$(readlink "$root/usr/bin/$name")" = coreutils ] &&
        [ -x "$root/usr/bin/$name" ] || {
        echo "coreutils: missing or incorrect GNU command: $name" >&2; exit 1;
    }
done < "$programs"
[ -f "$root/usr/libexec/coreutils/libstdbuf.so" ] &&
    [ ! -L "$root/usr/libexec/coreutils/libstdbuf.so" ] || {
    echo 'coreutils: missing stdbuf preload library' >&2; exit 1;
}
printf 'GNU coreutils complete installation: %s commands, PASS\n' "$(wc -l < "$programs" | tr -d ' ')"
