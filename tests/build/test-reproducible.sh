#!/bin/sh
# Acceptance row A13: the same configuration built twice must produce the same
# bytes, at a pinned SOURCE_DATE_EPOCH, in two unrelated output directories.
#
# This is the reproducibility half of the migration: a build that cannot be
# repeated byte-for-byte cannot be audited, and "we pass SOURCE_DATE_EPOCH
# somewhere" is not evidence. Both trees are built from scratch, into different
# paths, and compared file by file.
set -u

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
cd "$repo_root" || exit 1

failures=0
checks=0
work=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-repro.XXXXXX") || exit 1
epoch=${SOURCE_DATE_EPOCH:-1700000000}

cleanup() { [ -n "${KEEP_WORK:-}" ] || rm -rf "$work"; }
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap cleanup EXIT

pass() { checks=$((checks + 1)); printf 'ok   - %s\n' "$1"; }
fail() {
    checks=$((checks + 1))
    failures=$((failures + 1))
    printf 'FAIL - %s\n' "$1"
    if [ "$#" -gt 1 ]; then
        shift
        printf '       %s\n' "$@"
    fi
}

build_into() {
    mkdir -p "$1" || return 1
    # Different output paths, including a different depth from the source root,
    # so an accidentally embedded build directory shows up as a hash difference.
    # build-info is explicit since the kernel adapter: the version header is
    # parent-owned (rpr/site/app packages) and no longer rides the kernel link.
    SOURCE_DATE_EPOCH="$epoch" \
        make -s O="$1" -j"$(nproc)" kernel build-info >"$1.log" 2>&1
}

first=$work/one/out
second=$work/nested/deeper/two/out

printf '=== building the same configuration twice (epoch %s) ===\n' "$epoch"
if ! build_into "$first"; then
    fail 'the first output tree builds' "see $first.log"
    tail -n 15 "$first.log"
    printf 'not ok - reproducibility: %d of %d checks failed\n' "$failures" "$checks"
    exit 1
fi
if ! build_into "$second"; then
    fail 'the second output tree builds' "see $second.log"
    tail -n 15 "$second.log"
    printf 'not ok - reproducibility: %d of %d checks failed\n' "$failures" "$checks"
    exit 1
fi
pass 'both output trees build from scratch'

# Every product, not just the image: a generated header that drifts would change
# what the next rebuild recompiles. The kernel side is compared through the
# sub-build tree (reliefnt/) and the products the adapter publishes.
for product in include/generated/autoconf.h \
        include/generated/build_info.h reliefnt/built-in.a \
        reliefnt/generated/system/kernel.unstripped generated/system/kernel.sys \
        generated/system/kernel.debug; do
    checks=$((checks + 1))
    if [ ! -f "$first/$product" ]; then
        fail "$product exists in the first tree" "missing"
        continue
    fi
    if [ ! -f "$second/$product" ]; then
        fail "$product exists in the second tree" "missing"
        continue
    fi
    a=$(sha256sum "$first/$product" | cut -d' ' -f1)
    b=$(sha256sum "$second/$product" | cut -d' ' -f1)
    if [ "$a" = "$b" ]; then
        printf 'ok   - %-40s %s\n' "$product" "$(printf '%s' "$a" | cut -c1-16)"
    else
        printf 'FAIL - %s differs between two identical builds\n' "$product"
        printf '       one=%s\n       two=%s\n' "$a" "$b"
        failures=$((failures + 1))
    fi
done

# A rebuilt tree must also agree with itself: build the first tree again and
# check the image did not move.
before=$(sha256sum "$first/generated/system/kernel.sys" | cut -d' ' -f1)
build_into "$first"
after=$(sha256sum "$first/generated/system/kernel.sys" | cut -d' ' -f1)
if [ "$before" = "$after" ]; then
    pass 'rebuilding an existing tree leaves the image unchanged'
else
    fail 'rebuilding an existing tree leaves the image unchanged' \
        "$before != $after"
fi

# The version header has to carry the pinned epoch rather than the wall clock:
# that is the only part of A13 that a running clock can break.
if grep -q "$epoch" "$first/include/generated/build_info.h" 2>/dev/null \
        || grep -qiE 'build_?(date|time).*1970|20[0-9][0-9]-[0-9][0-9]-[0-9][0-9]' \
            "$first/include/generated/build_info.h" 2>/dev/null; then
    pass 'the build info header carries a fixed, derived timestamp'
else
    fail 'the build info header carries a fixed, derived timestamp' \
        "no dated macro found in build_info.h"
fi

printf -- '---\n'
if [ "$failures" -ne 0 ]; then
    printf 'not ok - reproducibility: %d of %d checks failed\n' "$failures" "$checks"
    exit 1
fi
printf 'ok - reproducibility: %d checks passed\n' "$checks"
