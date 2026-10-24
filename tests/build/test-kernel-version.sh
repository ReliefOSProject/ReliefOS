#!/bin/sh
# Check ReliefOS forwards version suffixes through its kernel adapter.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-kernel-version.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
make -s -C "$src" O="$work/out" defconfig >"$work/configure.log" 2>&1
make -s -n -C "$src" O="$work/out" EXTRAVERSION=-perf LOCALVERSION=-test kernel >"$work/plan"
grep -Fq "EXTRAVERSION='-perf' LOCALVERSION='-test'" "$work/plan"
if make -s -n -C "$src" O="$work/out" EXTRAVERSION='bad/flag' kernel >"$work/rejected.log" 2>&1; then
    echo 'FAIL - unsafe suffix reached the kernel adapter'
    exit 1
fi
printf 'kernel version adapter: suffix passthrough and validation passed\n'
