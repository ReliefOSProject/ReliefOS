#!/bin/sh
# Build an older published kernel in a scratch clone; never change the shared
# submodule HEAD or overwrite local Kbuild work. Release mismatch checks use an
# isolated parent gitlink fixture. usage: test-submodule-rollback-build.sh [SHA]
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
original_status=$(git -C "$reliefnt" status --porcelain)
original_checkout=$reliefnt
cleanup() { [ -n "${KEEP_WORK:-}" ] || rm -rf "$work"; }
trap cleanup EXIT INT TERM
git clone -q --no-checkout "$reliefnt" "$work/rollback-kernel" || exit 1
reliefnt=$work/rollback-kernel

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
if make -s -j"$(nproc)" O="$work/out" RELIEFNT_DIR="$reliefnt" kernel >"$work/build.log" 2>&1; then
    pass "make kernel builds at the rolled-back pin"
else
    fail "make kernel builds at the rolled-back pin" \
        "see $work/build.log" "$(tail -n 5 "$work/build.log")"
fi
missing=
for product in system/kernel.sys system/kernel.debug system/kerneldebug.sys; do
    [ -f "$work/out/generated/$product" ] || missing="$missing $product"
done
if [ -z "$missing" ]; then
    pass "all published kernel products exist at the rolled-back pin"
else
    fail "all published kernel products exist at the rolled-back pin" \
        "missing:$missing"
fi
# The rolled-back pin may still install legacy loadable driver modules into the
# sub-make install directory; the current adapter publishes only the three kernel
# products and leaves those unpublished.
if find "$work/out/kernel-install" -name '*.drv' 2>/dev/null | grep -q .; then
    printf 'note - rolled-back pin installs legacy .drv modules; they stay unpublished\n'
fi

printf '\n=== (2) release guard rejects a rollback mismatch in isolated metadata ===\n'
parent=$work/guard-parent
git init -q "$parent"
mkdir -p "$parent/kernel"
git clone -q "$reliefnt" "$parent/kernel/reliefnt"
git -C "$parent" -c user.name=fixture -c user.email=fixture@example.invalid update-index --add --cacheinfo "160000,$gitlink,kernel/reliefnt"
git -C "$parent" -c user.name=fixture -c user.email=fixture@example.invalid commit -q -m 'fixture pin'
if sh "$repo_root/tools/build/reliefnt-release-guard.sh" "$parent" "$parent/kernel/reliefnt" kernel/reliefnt >"$work/rpr.log" 2>&1; then
    fail 'release guard refuses the rollback mismatch' 'guard exited0'
elif grep -q "$gitlink" "$work/rpr.log" && grep -q "$target" "$work/rpr.log"; then
    pass 'release guard refuses rollback and names both SHAs'
else
    fail 'release guard names both SHAs' "$(cat "$work/rpr.log")"
fi
printf '\n=== (3) original checkout was never changed ===\n'
if [ "$(git -C "$original_checkout" rev-parse HEAD)" = "$orig" ] && [ "$(git -C "$original_checkout" status --porcelain)" = "$original_status" ]; then
    pass 'original kernel HEAD and local edits preserved'
else
    fail 'original kernel HEAD and local edits preserved'
fi

printf '\n'
if [ "$failures" = 0 ]; then
    printf 'test-submodule-rollback-build: all %s checks passed\n' "$checks"
else
    printf 'test-submodule-rollback-build: %s of %s checks failed\n' \
        "$failures" "$checks"
    exit 1
fi
