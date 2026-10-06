#!/bin/sh
# Old-SHA rollback BUILD (plan stage 5: "旧 SHA 回退"). Complements
# tests/build/test-submodule-contract.sh cases 3-4, which prove the release
# guard's pairing rules but deliberately never build (their fixture clone has
# no fetch cache). This test runs against the real worktree checkout:
#
#   1. detach the kernel submodule at an older published SHA (the documented
#      daily-dev state) and build the complete product set declared by that pin
#      into a fresh output directory. A pure kernel build must not require a
#      UAPI whitelist newer than the pinned kernel provides;
#   2. while rolled back, refuse release flows: `make rpr-pages` must fail
#      with a message naming both the gitlink SHA and the rolled-back SHA;
#   3. restore the original checkout on every exit path.
#
# usage: test-submodule-rollback-build.sh [OLD_SHA]   (default: 5cc9621,
# the previous release pin). The worktree submodule is left as found.
set -u
LC_ALL=C
export LC_ALL

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P) || exit 1
cd "$repo_root" || exit 1
reliefnt=$repo_root/kernel/reliefnt
target=${1:-5cc9621}

failures=0
checks=0
pass() { checks=$((checks + 1)); printf 'ok   - %s\n' "$1"; }
fail() {
    checks=$((checks + 1))
    failures=$((failures + 1))
    printf 'FAIL - %s\n' "$1"
    if [ "$#" -gt 1 ]; then shift; printf '       %s\n' "$@"; fi
}

if ! orig=$(git -C "$reliefnt" rev-parse HEAD 2>/dev/null); then
    printf 'FAIL - kernel submodule not initialized: %s\n' "$reliefnt" >&2
    exit 1
fi
if ! git -C "$reliefnt" rev-parse --verify -q "$target^{commit}" >/dev/null; then
    printf 'usage error: %s is not a commit in %s\n' "$target" "$reliefnt" >&2
    exit 2
fi
target=$(git -C "$reliefnt" rev-parse "$target^{commit}")
if [ "$target" = "$orig" ]; then
    printf 'usage error: %s is the current checkout; pass a different SHA\n' "$target" >&2
    exit 2
fi
gitlink=$(git -C "$repo_root" ls-tree HEAD -- kernel/reliefnt | awk '{print $3}')
if [ -z "$gitlink" ]; then
    printf 'FAIL - no committed kernel/reliefnt gitlink in HEAD\n' >&2
    exit 1
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-rollback-build.XXXXXX") || exit 1
restored=0
cleanup() {
    if [ "$restored" = 0 ]; then
        git -C "$reliefnt" checkout -q --detach "$orig" 2>/dev/null
        git -C "$reliefnt" submodule update --init --recursive >/dev/null 2>&1
    fi
    [ -n "${KEEP_WORK:-}" ] || rm -rf "$work"
}
trap cleanup EXIT INT TERM

printf '=== (1) rollback build at %s (gitlink pins %s) ===\n' \
    "$(git -C "$reliefnt" rev-parse --short "$target")" \
    "$(git -C "$repo_root" rev-parse --short "$gitlink" 2>/dev/null || echo "$gitlink")"
if git -C "$reliefnt" checkout -q --detach "$target" 2>"$work/checkout.log"; then
    pass "the kernel checkout is detached at the rollback SHA"
else
    fail "the kernel checkout is detached at the rollback SHA" \
        "$(cat "$work/checkout.log")"
    printf '%s: aborting\n' "$0" >&2
    exit 1
fi
if git -C "$reliefnt" submodule update --init --recursive >"$work/sub-update.log" 2>&1; then
    pass "nested kernel build dependencies are initialized at the rollback SHA"
else
    fail "nested kernel build dependencies are initialized at the rollback SHA" \
        "see $work/sub-update.log" "$(tail -n 5 "$work/sub-update.log")"
fi
if make -s -C "$reliefnt" O="$work/out/reliefnt" fetch >"$work/fetch.log" 2>&1; then
    pass "locked kernel build dependencies are available at the rollback SHA"
else
    fail "locked kernel build dependencies are available at the rollback SHA" \
        "see $work/fetch.log" "$(tail -n 5 "$work/fetch.log")"
fi
if make -s -j"$(nproc)" O="$work/out" kernel >"$work/build.log" 2>&1; then
    pass "make kernel builds at the rolled-back pin"
else
    fail "make kernel builds at the rolled-back pin" \
        "see $work/build.log" "$(tail -n 5 "$work/build.log")"
fi
missing=
for product in system/kernel.sys system/kernel.debug system/kerneldebug.sys \
        boot/loader.elf; do
    [ -f "$work/out/generated/$product" ] || missing="$missing $product"
done
rollback_drivers=
if [ -f "$reliefnt/mk/boot.mk" ]; then
    rollback_drivers=$(sed -n 's/^DRIVER_NAMES[[:space:]]*:=\(.*\)$/\1/p' \
        "$reliefnt/mk/boot.mk" | tr '\n' ' ')
fi
for driver in $rollback_drivers; do
    product="drivers/$driver.drv"
    [ -f "$work/out/generated/$product" ] || missing="$missing $product"
done
if [ -z "$rollback_drivers" ]; then
    fail "the rollback pin declares a driver product set" \
        "could not read DRIVER_NAMES from $reliefnt/mk/boot.mk"
elif [ -z "$missing" ]; then
    pass "all kernel products declared by the rollback pin are published"
else
    fail "all kernel products declared by the rollback pin are published" \
        "missing:$missing" "drivers:$rollback_drivers"
fi
if printf '%s\n' "$rollback_drivers" | grep -qw hda; then
    pass "the rollback pin publishes the HDA driver"
else
    printf 'note - rollback pin predates HDA; its declared product set is accepted without hda.drv\n'
fi

printf '\n=== (2) release flows refuse the rolled-back state (gitlink mismatch) ===\n'
if make -s O="$work/out" rpr-pages >"$work/rpr.log" 2>&1; then
    fail "make rpr-pages refuses a rolled-back checkout" 'make exited 0'
else
    pass "make rpr-pages refuses a rolled-back checkout"
fi
if grep -q "$gitlink" "$work/rpr.log" && grep -q "$target" "$work/rpr.log"; then
    pass "the refusal names both SHAs (gitlink and rollback HEAD)"
else
    fail "the refusal names both SHAs (gitlink and rollback HEAD)" \
        "want: $gitlink and $target" "$(head -c 300 "$work/rpr.log")"
fi

printf '\n=== (3) restore the original checkout ===\n'
if git -C "$reliefnt" checkout -q --detach "$orig" 2>"$work/restore.log"; then
    restored=1
    pass "the kernel checkout is back at the original SHA"
else
    fail "the kernel checkout is back at the original SHA" \
        "$(cat "$work/restore.log")"
fi
git -C "$reliefnt" submodule update --init --recursive >>"$work/restore.log" 2>&1
if [ -z "$(git -C "$reliefnt" status --porcelain)" ] &&
        [ "$(git -C "$reliefnt" rev-parse HEAD)" = "$orig" ]; then
    pass "the submodule is clean at the original SHA after restore"
else
    fail "the submodule is clean at the original SHA after restore" \
        "$(git -C "$reliefnt" status --porcelain)" \
        "HEAD=$(git -C "$reliefnt" rev-parse HEAD) want=$orig"
fi

printf '\n'
if [ "$failures" = 0 ]; then
    printf 'test-submodule-rollback-build: all %s checks passed\n' "$checks"
else
    printf 'test-submodule-rollback-build: %s of %s checks failed\n' \
        "$failures" "$checks"
    exit 1
fi
