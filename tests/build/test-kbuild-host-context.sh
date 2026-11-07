#!/bin/sh
# A target-tool identity exported by the product Kbuild must not turn a
# host-only native Kbuild invocation into a target archive build.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
tmp_base=${TMPDIR:-$root/out/tmp}
mkdir -p "$tmp_base"
work=$(mktemp -d "$tmp_base/reliefos-kbuild-host-context.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

if ! RELIEFOS_AR_ID=./meta/target-ar.identity \
    make --no-print-directory -C "$root" O="$work/out" -j4 test-tools \
    >"$work/test-tools.log" 2>&1; then
    cat "$work/test-tools.log" >&2
    echo 'FAIL - fresh native test-tools build inherited a target AR identity into host Kbuild' >&2
    exit 1
fi

for product in \
    scripts/basic/fixdep \
    host/tests/test_common host/tests/test_json host/tests/test_locale_conf \
    host/tests-sanitised/test_common host/tests-sanitised/test_json \
    host/tests-sanitised/test_locale_conf; do
    test -x "$work/out/$product" || {
        echo "FAIL - fresh test-tools build omitted $product" >&2
        exit 1
    }
done

printf 'fresh test-tools build passed with inherited RELIEFOS_AR_ID; fixdep and both host test contexts were produced\n'
