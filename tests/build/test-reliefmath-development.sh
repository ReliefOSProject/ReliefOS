#!/bin/sh
# Exercises missing-member rebuilds in this task's generated development tree.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
cd "$src"
output=${1:-${RELIEFOS_TEST_BUILD_ROOT:-out/x86_64/release}}
case $output in out/*|"$src"/out/*) ;; *) echo 'expected repository out directory' >&2; exit 2;; esac
root=$output/upstream/reliefmath-development
make O="$output" reliefmath-development
member=usr/include/freetype2/freetype/ftoutln.h
grep -Fxq "$member" "$root/.members"
work=$(mktemp -d "$src/out/tmp/reliefmath-dev-member.XXXXXX")
restore() {
    if [ -f "$work/member" ] && [ ! -e "$root/$member" ]; then mv "$work/member" "$root/$member"; fi
    rm -rf "$work"
}
trap restore EXIT HUP INT TERM
mv "$root/$member" "$work/member"
if make -q O="$output" reliefmath-development; then echo 'missing declared header did not invalidate stage' >&2; exit 1; fi
make O="$output" reliefmath-development
cmp "$root/$member" "$work/member"
test -s "$root/.members"
printf 'ReliefMath complete development-member rebuild passed\n'
