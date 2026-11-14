#!/bin/sh
# Requires a real configured O; never edits project sources.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
out=${1:?usage: upstream-incremental.sh O}
make_out=$out
out=$(CDPATH= cd -- "$out" && pwd)
work=$(mktemp -d "$out/meta/upstream-incremental.XXXXXX")
trap 'rm -rf "$work"' EXIT
snapshot() {
 for pkg in libmd libbsd util-linux sudo shadow exfatprogs; do
  find "$out/upstream/$pkg/root" -type f -printf '%p %T@ %s\n'
 done
 find "$out/upstream/apk" "$out/upstream/runtime-development" -type f -printf '%p %T@ %s\n'
 find "$out/userland" -type f -printf '%p %T@ %s\n'
 if [ -d "$out/resources" ]; then find "$out/resources" -type f -printf '%p %T@ %s\n'; fi
}
make -C "$src" O="$make_out" leonos-upstream -j8 > "$work/baseline.log" 2>&1 || { cat "$work/baseline.log" >&2; exit 1; }
snapshot > "$work/before.unsorted"
LC_ALL=C sort "$work/before.unsorted" > "$work/before"
make -C "$src" O="$make_out" leonos-upstream -j8 > "$work/noop.log" 2>&1 || { cat "$work/noop.log" >&2; exit 1; }
snapshot > "$work/after.unsorted"
LC_ALL=C sort "$work/after.unsorted" > "$work/after"
cmp "$work/before" "$work/after"
# Exercise the complete install manifest, beyond the primary ELF outputs.
set -- "$out/upstream/apk/packages/dosfstools-doc-"[0-9]*.apk
test "$#" = 1
secondary=$1
test -f "$secondary"
rm "$secondary"
make -C "$src" O="$make_out" upstream-dosfstools -j8 > "$work/restore.log" 2>&1 || { cat "$work/restore.log" >&2; exit 1; }
test -f "$secondary"
# Development inputs are isolated too, and every selected member must be
# recovered even when it is not one of the Make rule's primary headers.
secondary=$out/upstream/util-linux/root/usr/include/libeconf_ext.h
test -s "$secondary"
expected=$(sha256sum "$secondary")
rm "$secondary"
make -C "$src" O="$make_out" util-linux-development -j8 > "$work/restore-util-linux.log" 2>&1 || { cat "$work/restore-util-linux.log" >&2; exit 1; }
test "$(sha256sum "$secondary")" = "$expected"
printf '%s\n' 'upstream no-op mtimes and secondary-output restoration: PASS'
