#!/bin/sh
# Exercise the standalone kernel's actual version renderer and procfs fixture.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
kernel=$src/kernel/reliefnt
work=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-kernel-version.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
${HOSTCC:-cc} -std=c11 -O2 -Wall -Wextra -Werror -I"$kernel" \
    "$kernel/tools/host/version/reliefos-version.c" \
    "$kernel/tools/host/common/io.c" "$kernel/tools/host/common/buffer.c" -o "$work/version"
render() {
    "$work/version" --version-file "$kernel/configs/build-version" \
        --source-id abc123 --epoch 1700000000 --output "$work/build.h" "$@"
}
render
grep -qx '#define RELIEFOS_KERNEL_VERSION "5.1.0"' "$work/build.h"
render --extra-version -perf --local-version -test
grep -qx '#define RELIEFOS_KERNEL_VERSION "5.1.0-perf-test"' "$work/build.h"
grep -qx '#define RELIEFOS_KERNEL_VERSION_MAJOR 5' "$work/build.h"
grep -qx '#define RELIEFOS_KERNEL_VERSION_MINOR 1' "$work/build.h"
grep -qx '#define RELIEFOS_KERNEL_VERSION_PATCH 0' "$work/build.h"
before=$(stat -c %y "$work/build.h")
render --extra-version -perf --local-version -test
test "$before" = "$(stat -c %y "$work/build.h")"
cp "$work/build.h" "$work/expected.h"
for suffix in 'bad/flag' 'bad"flag' 'abcdefghijklmnopqrstuvwxyz0123456789'; do
    if render --extra-version "$suffix" >"$work/rejected.log" 2>&1; then
        echo "FAIL - unsafe or oversized kernel suffix accepted: $suffix"
        exit 1
    fi
    cmp "$work/expected.h" "$work/build.h"
done
render --extra-version '' --local-version ''
grep -qx '#define RELIEFOS_KERNEL_VERSION "5.1.0"' "$work/build.h"
make -s -n -C "$src" O="$work/out" EXTRAVERSION=-perf LOCALVERSION=-test kernel >"$work/plan"
grep -Fq "EXTRAVERSION='-perf' LOCALVERSION='-test'" "$work/plan"
if make -s -n -C "$src" O="$work/out" EXTRAVERSION='bad/flag' kernel >"$work/rejected.log" 2>&1; then
    echo 'FAIL - unsafe suffix reached the kernel adapter'
    exit 1
fi
${HOSTCC:-cc} -std=c11 -O1 -g -fsanitize=address,undefined \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -I"$kernel/include" -I"$src/include" -I"$kernel/include/uapi" \
    -I"$kernel/kernel/reliefnt/include" "$src/tools/tests/procfs_directories_test.c" -o "$work/procfs"
"$work/procfs"
printf 'kernel version: base release, suffixes, numeric ABI, no-op, reset, validation, adapter and procfs passed\n'
