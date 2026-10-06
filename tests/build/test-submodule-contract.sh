#!/bin/sh
# Contract tests for the ReliefNT submodule integration (separation phase 5):
#
#   1. an uninitialized kernel checkout fails `make kernel` with the adapter's
#      actionable error naming RELIEFNT_DIR and kernel/reliefnt;
#   2. a dirty kernel/reliefnt submodule is allowed for development builds
#      (`make kernel` still builds) but refuses the release target
#      (`make rpr-pages` fails with the dirty message) -- design §8;
#   3. a gitlink mismatch (submodule checked out at a different published SHA)
#      refuses release builds with a message naming both SHAs -- simulated in a
#      scratch clone, never the real worktree;
#   4. old-SHA rollback: a scratch clone whose gitlink consistently pins an
#      older published kernel SHA passes the release guard. Since 3430c76 the
#      pinned pair (HEAD / HEAD~1) are two complete kernel snapshots, so cases
#      3-4 exercise a real dual-SHA pin. The matching rollback-BUILD (kernel
#      product set from an older pin, in the real worktree) lives in
#      tests/build/test-submodule-rollback-build.sh; this contract test never
#      builds (its fixture clone has no fetch cache). Boot-time kernel update
#      rollback is covered by the §5.8 guest tests instead.
set -u
LC_ALL=C
export LC_ALL

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
cd "$repo_root" || exit 1

reliefnt=${RELIEFNT_DIR:-${NTCLKS_DIR:-$repo_root/kernel/reliefnt}}
gitlink_path=kernel/reliefnt

work=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-submodule.XXXXXX") || exit 1
failures=0
checks=0

cleanup() {
    # Always leave the real submodule clean.
    rm -f "$repo_root/kernel/reliefnt/P5_CONTRACT_DIRTY_PROBE"
    [ -n "${KEEP_WORK:-}" ] || rm -rf "$work"
}
trap cleanup EXIT INT TERM

pass() { checks=$((checks + 1)); printf 'ok   - %s\n' "$1"; }
fail() {
    checks=$((checks + 1))
    failures=$((failures + 1))
    printf 'FAIL - %s\n' "$1"
    if [ "$#" -gt 1 ]; then shift; printf '       %s\n' "$@"; fi
}

printf '=== (1) an uninitialized kernel checkout fails with an actionable error ===\n'
mkdir -p "$work/uninitialized"
if make -s O="$work/out" RELIEFNT_DIR="$work/uninitialized" kernel \
        >"$work/uninitialized.log" 2>&1; then
    fail "make kernel refuses an uninitialized kernel checkout" 'make exited 0'
else
    pass "make kernel refuses an uninitialized kernel checkout"
fi
if grep -q 'RELIEFNT_DIR' "$work/uninitialized.log" &&
        grep -q 'kernel/reliefnt' "$work/uninitialized.log"; then
    pass "the uninitialized-checkout error gives ReliefNT setup guidance"
else
    fail "the uninitialized-checkout error gives ReliefNT setup guidance" \
        "$(head -c 200 "$work/uninitialized.log")"
fi

printf '\n=== (2) a dirty submodule builds (dev) but refuses release ===\n'
probe=$repo_root/kernel/reliefnt/P5_CONTRACT_DIRTY_PROBE
: >"$probe"
if [ -n "$(git -C "$reliefnt" status --porcelain)" ]; then
    pass "the probe file dirties the submodule"
else
    fail "the probe file dirties the submodule" 'git status --porcelain stayed empty'
fi
if make -s -j"$(nproc)" O="$work/out" kernel >"$work/dirty-kernel.log" 2>&1; then
    pass "make kernel still builds with a dirty submodule (dev contract)"
else
    fail "make kernel still builds with a dirty submodule (dev contract)" \
        "see $work/dirty-kernel.log" "$(tail -n 5 "$work/dirty-kernel.log")"
fi
if make -s O="$work/out" rpr-pages >"$work/dirty-rpr.log" 2>&1; then
    fail "make rpr-pages refuses a dirty submodule" 'make exited 0'
else
    pass "make rpr-pages refuses a dirty submodule"
fi
if grep -q 'dirty' "$work/dirty-rpr.log"; then
    pass "the refusal message says the submodule is dirty"
else
    fail "the refusal message says the submodule is dirty" \
        "$(head -c 200 "$work/dirty-rpr.log")"
fi
rm -f "$probe"
if [ -z "$(git -C "$reliefnt" status --porcelain)" ]; then
    pass "the submodule is clean again after restore"
else
    fail "the submodule is clean again after restore" \
        "$(git -C "$reliefnt" status --porcelain)"
fi

printf '\n=== (3) a gitlink mismatch refuses release (scratch clone) ===\n'
clone=$work/clone
if git clone -q "$repo_root" "$clone" 2>"$work/clone.log"; then
    pass "a scratch clone of the repository exists"
else
    fail "a scratch clone of the repository exists" "$(cat "$work/clone.log")"
    printf '%s: aborting\n' "$0" >&2
    exit 1
fi
# Record a committed kernel gitlink in the fixture whatever the parent's own
# commit state is, then check out a different published SHA below it. When the
# clone's HEAD already records the same pin the re-registered index entry
# matches HEAD exactly and a commit would be empty: the pin is already the
# committed state the fixture needs, so only commit a real difference.
git -C "$clone" rm -r -q --cached kernel >/dev/null 2>&1 || true
rm -rf "$clone/kernel"
pin=$(git -C "$reliefnt" rev-parse HEAD)
other=$(git -C "$reliefnt" rev-parse HEAD~1 2>/dev/null || true)
git -C "$clone" -c user.name=fixture -c user.email=fixture@example.invalid \
    update-index --add --cacheinfo "160000,$pin,$gitlink_path" || exit 1
if ! git -C "$clone" diff --cached --quiet; then
    git -C "$clone" -c user.name=fixture -c user.email=fixture@example.invalid \
        commit -q -m 'fixture: record kernel gitlink' || exit 1
fi
# The fixture must exercise the tree under test, including changes not yet
# committed: sync the guard and its wiring into the fixture (same idea as the
# source copies in test-kernel-adapter.sh fixtures). The adapter's parse guard
# in mk/kernel.mk is part of that wiring: it must not shadow the release
# guard's dual-SHA refusal for an initialized checkout at another published
# SHA.
cp "$repo_root/tools/build/reliefnt-release-guard.sh" "$clone/tools/build/" || exit 1
cp "$repo_root/mk/rpr.mk" "$clone/mk/rpr.mk" || exit 1
cp "$repo_root/mk/kernel.mk" "$clone/mk/kernel.mk" || exit 1
cp "$repo_root/mk/headers.mk" "$clone/mk/headers.mk" || exit 1
mkdir -p "$clone/mk/components"
cp "$repo_root/mk/components/audio.mk" "$clone/mk/components/audio.mk" || exit 1
cp "$repo_root/Makefile" "$clone/Makefile" || exit 1
if [ -z "${other:-}" ]; then
    printf 'skip - gitlink mismatch case: the kernel repository has no older\n'
    printf '       published SHA to check out under the fixture gitlink\n'
elif git clone -q --no-checkout "$repo_root/kernel/reliefnt" \
        "$clone/kernel/reliefnt" 2>"$work/sub-clone.log"; then
    git -C "$clone/kernel/reliefnt" checkout -q --detach "$other" 2>/dev/null
    # The fixture's make needs the config chain: seed the two submodules it
    # parses/builds from the (already initialized) worktree checkouts.
    # git >= 2.38 blocks the file transport for submodules by default.
    cp "$repo_root/.gitmodules" "$clone/.gitmodules"
    git -C "$clone" config submodule.third_party/zlib.url "$repo_root/third_party/zlib"
    git -C "$clone" config submodule.third_party/kconfig-frontends.url \
        "$repo_root/third_party/kconfig-frontends"
    git -c protocol.file.allow=always -C "$clone" submodule update -q --init \
        third_party/zlib third_party/kconfig-frontends \
        2>"$work/fixture-submodules.log"
    if make -s -C "$clone" O="$work/clone-out" rpr-pages \
            >"$work/mismatch.log" 2>&1; then
        fail "make rpr-pages refuses a gitlink mismatch" 'make exited 0'
    else
        pass "make rpr-pages refuses a gitlink mismatch"
    fi
    if grep -q "$pin" "$work/mismatch.log" && grep -q "$other" "$work/mismatch.log"; then
        pass "the refusal message names both SHAs (gitlink=$pin HEAD=$other)"
    else
        fail "the refusal message names both SHAs" \
            "want: $pin and $other" "$(head -c 300 "$work/mismatch.log")"
    fi
else
    fail "the fixture submodule checks out a different published SHA" \
        "$(cat "$work/sub-clone.log" 2>/dev/null)"
fi

printf '\n=== (4) old-SHA rollback: guard accepts a consistent older pin ===\n'
if [ -n "${other:-}" ] && { [ -d "$clone/kernel/reliefnt/.git" ] || [ -f "$clone/kernel/reliefnt/.git" ]; }; then
    # Roll the fixture's gitlink back to the older SHA so gitlink and checkout
    # agree again: the guard must NOT refuse this (rollback pinning works).
    git -C "$clone" -c user.name=fixture -c user.email=fixture@example.invalid \
        update-index --cacheinfo "160000,$other,$gitlink_path" || exit 1
    if ! git -C "$clone" diff --cached --quiet; then
        git -C "$clone" -c user.name=fixture -c user.email=fixture@example.invalid \
            commit -q -m 'fixture: roll kernel gitlink back' || exit 1
    fi
    if sh "$repo_root/tools/build/reliefnt-release-guard.sh" "$clone" \
            "$clone/kernel/reliefnt" "$gitlink_path" >"$work/rollback.log" 2>&1; then
        pass "a consistent older kernel pin passes the release guard"
    else
        fail "a consistent older kernel pin passes the release guard" \
            "$(head -c 300 "$work/rollback.log")"
    fi
    printf 'note - the full old-SHA rollback BUILD runs in\n'
    printf '       tests/build/test-submodule-rollback-build.sh against the real\n'
    printf '       worktree (this fixture clone has no fetch cache to build\n'
    printf '       with). The guard/rollback pinning path is covered by the\n'
    printf '       check above.\n'
else
    printf 'skip - old-SHA rollback case: the kernel repository has no older\n'
    printf '       published SHA to pin here; with one, the pinning check above\n'
    printf '       and tests/build/test-submodule-rollback-build.sh cover it\n'
fi

printf '\n%d checks, %d failures\n' "$checks" "$failures"
[ "$failures" -eq 0 ]
