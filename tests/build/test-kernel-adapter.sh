#!/bin/sh
# Contract tests for the ReliefNT kernel adapter (mk/kernel.mk, mk/headers.mk).
#
# The kernel products are built by the standalone checkout and published here;
# this suite covers the adapter's failure and recovery surface: a missing
# checkout fails with an actionable error, a dirty checkout is rebuilt and
# re-published, a deleted product is restored, parallel invocations on separate
# output directories succeed, and a build failure in the checkout surfaces the
# sub-make error and publishes no half-products. The destructive cases run
# against a scratch copy of the checkout (a fixture), never the real one.
set -u
LC_ALL=C
export LC_ALL

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
cd "$repo_root" || exit 1

# The kernel checkout under test (env-overridable, see tests/build/test-incremental.sh).
reliefnt=${RELIEFNT_DIR:-${NTCLKS_DIR:-$repo_root/kernel/reliefnt}}

work=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-adapter.XXXXXX") || exit 1
O="$work/out"
failures=0
checks=0

cleanup() {
    [ -n "${KEEP_WORK:-}" ] || rm -rf "$work"
}
trap cleanup EXIT INT TERM

advance_clock() { sleep 1; }

pass() { checks=$((checks + 1)); printf 'ok   - %s\n' "$1"; }
fail() {
    checks=$((checks + 1))
    failures=$((failures + 1))
    printf 'FAIL - %s\n' "$1"
    if [ "$#" -gt 1 ]; then shift; printf '       %s\n' "$@"; fi
}

adapter_contract() {
    printf '=== ReliefNT adapter naming and compatibility contract ===\n'
    mkdir -p "$work/new-kernel" "$work/old-kernel"
    : > "$work/new-kernel/Makefile"
    : > "$work/old-kernel/Makefile"
    if make -s O="$work/contract-config" defconfig >"$work/contract-defconfig.log" 2>&1; then
        pass 'contract configuration is isolated under a temporary O'
    else
        fail 'contract configuration is isolated under a temporary O' \
            "$(head -c 300 "$work/contract-defconfig.log")"
    fi

    if make -s -n O="$work/plan-new" \
            RELIEFNT_DIR="$work/new-kernel" RELIEFNT_O="$work/new-out" kernel \
            >"$work/plan-new.log" 2>&1 &&
            grep -Fq -- "-C '$work/new-kernel' O='$work/new-out'" "$work/plan-new.log"; then
        pass 'RELIEFNT_DIR and RELIEFNT_O control the delegated build'
    else
        fail 'RELIEFNT_DIR and RELIEFNT_O control the delegated build' \
            "$(head -c 300 "$work/plan-new.log")"
    fi

    if env -u RELIEFNT_DIR make -s -n O="$work/plan-old" \
            NTCLKS_DIR="$work/old-kernel" NTCLKS_O="$work/old-out" kernel \
            >"$work/plan-old.log" 2>&1 &&
            grep -Fq -- "-C '$work/old-kernel' O='$work/old-out'" "$work/plan-old.log"; then
        pass 'legacy NTCLKS_DIR and NTCLKS_O remain accepted as inputs'
    else
        fail 'legacy NTCLKS_DIR and NTCLKS_O remain accepted as inputs' \
            "$(head -c 300 "$work/plan-old.log")"
    fi

    if make -s -n O="$work/plan-priority" \
            RELIEFNT_DIR="$work/new-kernel" RELIEFNT_O="$work/new-out" \
            NTCLKS_DIR="$work/old-kernel" NTCLKS_O="$work/old-out" kernel \
            >"$work/plan-priority.log" 2>&1 &&
            grep -Fq -- "-C '$work/new-kernel' O='$work/new-out'" "$work/plan-priority.log" &&
            ! grep -Fq -- "$work/old-kernel" "$work/plan-priority.log" &&
            ! grep -Fq -- "$work/old-out" "$work/plan-priority.log"; then
        pass 'new adapter variables take priority when both spellings are set'
    else
        fail 'new adapter variables take priority when both spellings are set' \
            "$(head -c 300 "$work/plan-priority.log")"
    fi

    if make -s -n O="$work/plan-missing" \
            RELIEFNT_DIR="$work/kernel/reliefnt" kernel >"$work/plan-missing.log" 2>&1; then
        fail 'a missing kernel checkout fails with ReliefNT setup guidance' 'make exited 0'
    elif grep -Fq 'kernel/reliefnt' "$work/plan-missing.log" &&
            grep -Fq 'RELIEFNT_DIR' "$work/plan-missing.log"; then
        pass 'a missing kernel checkout fails with ReliefNT setup guidance'
    else
        fail 'a missing kernel checkout fails with ReliefNT setup guidance' \
            "$(head -c 300 "$work/plan-missing.log")"
    fi

    for goal in reliefnt-fetch ntclks-fetch; do
        if make -s -n O="$work/fetch-$goal" \
                RELIEFNT_DIR="$work/new-kernel" "$goal" \
                >"$work/fetch-$goal.log" 2>&1; then
            pass "make $goal is available"
        else
            fail "make $goal is available" "$(head -c 300 "$work/fetch-$goal.log")"
        fi
    done

    guard="$repo_root/tools/build/reliefnt-release-guard.sh"
    fixture="$work/release-guard"
    source="$fixture/source"
    parent="$fixture/parent"
    mkdir -p "$fixture"
    git init -q --initial-branch=main "$source"
    git -C "$source" config user.name fixture
    git -C "$source" config user.email fixture@example.invalid
    printf 'first\n' > "$source/payload"
    git -C "$source" add payload
    git -C "$source" commit -q -m first
    matching=$(git -C "$source" rev-parse HEAD)
    git init -q --initial-branch=main "$parent"
    git -C "$parent" config user.name fixture
    git -C "$parent" config user.email fixture@example.invalid
    mkdir -p "$parent/kernel"
    git clone -q --no-hardlinks "$source" "$parent/kernel/reliefnt"
    git -C "$parent" config -f .gitmodules submodule.kernel/reliefnt.path kernel/reliefnt
    git -C "$parent" config -f .gitmodules submodule.kernel/reliefnt.url \
        https://github.com/ReliefOSProject/ReliefNT.git
    git -C "$parent" add .gitmodules
    git -C "$parent" update-index --add --cacheinfo "160000,$matching,kernel/reliefnt"
    git -C "$parent" commit -q -m 'fixture: record ReliefNT gitlink'

    if sh "$guard" "$parent" "$parent/kernel/reliefnt" kernel/reliefnt \
            >"$work/guard-clean.log" 2>&1; then
        pass 'release guard accepts a clean checkout at the recorded gitlink'
    else
        fail 'release guard accepts a clean checkout at the recorded gitlink' \
            "$(head -c 300 "$work/guard-clean.log")"
    fi

    : > "$parent/kernel/reliefnt/DIRTY_PROBE"
    if sh "$guard" "$parent" "$parent/kernel/reliefnt" kernel/reliefnt \
            >"$work/guard-dirty.log" 2>&1; then
        fail 'release guard rejects a dirty ReliefNT checkout' 'guard exited 0'
    elif grep -qi 'dirty' "$work/guard-dirty.log"; then
        pass 'release guard rejects a dirty ReliefNT checkout'
    else
        fail 'release guard rejects a dirty ReliefNT checkout' \
            "$(head -c 300 "$work/guard-dirty.log")"
    fi
    rm "$parent/kernel/reliefnt/DIRTY_PROBE"

    printf 'second\n' >> "$source/payload"
    git -C "$source" add payload
    git -C "$source" commit -q -m second
    mismatched=$(git -C "$source" rev-parse HEAD)
    git -C "$parent/kernel/reliefnt" fetch -q origin main
    git -C "$parent/kernel/reliefnt" checkout -q --detach "$mismatched"
    if sh "$guard" "$parent" "$parent/kernel/reliefnt" kernel/reliefnt \
            >"$work/guard-mismatch.log" 2>&1; then
        fail 'release guard rejects a gitlink SHA mismatch' 'guard exited 0'
    elif grep -Fq "$matching" "$work/guard-mismatch.log" &&
            grep -Fq "$mismatched" "$work/guard-mismatch.log"; then
        pass 'release guard reports both SHAs when they differ'
    else
        fail 'release guard reports both SHAs when they differ' \
            "$(head -c 300 "$work/guard-mismatch.log")"
    fi
}

adapter_contract
if [ "${RELIEFNT_CONTRACT_ONLY:-0}" = 1 ]; then
    printf '\n%s: %d checks, %d failures\n' 'test-kernel-adapter contract' "$checks" "$failures"
    [ "$failures" -eq 0 ] || exit 1
    exit 0
fi

build() {
    # $1 = log file, $2 = output directory, rest = extra make arguments.
    logfile=$1
    shift
    outdir=$1
    shift
    make -s -j"$(nproc)" O="$outdir" "$@" kernel >"$logfile" 2>&1
}

printf '=== (a) a missing kernel checkout fails with an actionable error ===\n'
for goal in kernel headers_install; do
    if make -s O="$work/missing" RELIEFNT_DIR="$work/no-such-checkout" "$goal" \
            >"$work/missing-$goal.log" 2>&1; then
        fail "missing checkout refuses '$goal'" 'make exited 0'
    else
        pass "missing checkout refuses '$goal'"
    fi
    if grep -q 'RELIEFNT_DIR' "$work/missing-$goal.log" &&
            grep -Fq 'git submodule update --init --recursive' "$work/missing-$goal.log"; then
        pass "the '$goal' error gives ReliefNT initialization guidance"
    else
        fail "the '$goal' error gives ReliefNT initialization guidance" \
            "$(head -c 200 "$work/missing-$goal.log")"
    fi
done

printf '\n=== (b) a dirty checkout source rebuilds and re-publishes ===\n'
if ! build "$work/base.log" "$O"; then
    fail 'the baseline kernel build succeeds' "see $work/base.log"
    tail -n 15 "$work/base.log" | sed 's/^/       | /'
    printf '%s: aborting\n' "$0" >&2
    exit 1
fi
pass 'the baseline kernel build succeeds'
expected_manifest="$work/expected-products"
actual_manifest="$work/actual-products"
printf '%s\n' kernel.debug kernel.sys kerneldebug.sys loader.elf \
    | LC_ALL=C sort > "$expected_manifest"
awk '/^artifacts:$/ { artifacts=1; next } artifacts { print $2 }' \
    "$O/kernel-install/manifest.txt" | LC_ALL=C sort > "$actual_manifest"
if cmp -s "$expected_manifest" "$actual_manifest"; then
    pass 'kernel manifest contains the four required product names'
else
    fail 'kernel manifest contains the four required product names' \
        "want: $(tr '\n' ' ' < "$expected_manifest")" \
        "have: $(tr '\n' ' ' < "$actual_manifest")"
fi
find "$O/generated" -type f -printf '%p %T@\n' | LC_ALL=C sort >"$work/published-before"
loader_hash_before=$(sha256sum "$O/generated/boot/loader.elf" | cut -d' ' -f1)
kerneldebug_hash_before=$(sha256sum "$O/generated/system/kerneldebug.sys" | cut -d' ' -f1)

advance_clock
touch "$reliefnt/kernel/reliefnt/futex.c"
if build "$work/dirty.log" "$O"; then
    pass 'a touched checkout source is rebuilt'
else
    fail 'a touched checkout source is rebuilt' 'make failed'
fi
if grep -qF -- "$reliefnt/kernel/reliefnt/futex.c" "$work/dirty.log" &&
        grep -qE '^  (LD|IMAGE) ' "$work/dirty.log"; then
    pass 'the sub-make relinks and refreshes the affected products'
else
    fail 'the sub-make relinks and refreshes the affected products' \
        "$(grep -E '^  (CC|LD|IMAGE) ' "$work/dirty.log" | head -3 | tr '\n' ' ')"
fi
if grep -qE 'loader\.elf|kerneldebug\.sys' "$work/dirty.log"; then
    fail 'a kernel source touch leaves the loader and debug module alone' \
        "$(grep -E 'loader\.elf|kerneldebug\.sys' "$work/dirty.log" | head -2 | tr '\n' ' ')"
else
    pass 'a kernel source touch leaves the loader and debug module alone'
fi
if [ "$loader_hash_before" = "$(sha256sum "$O/generated/boot/loader.elf" | cut -d' ' -f1)" ] &&
        [ "$kerneldebug_hash_before" = "$(sha256sum "$O/generated/system/kerneldebug.sys" | cut -d' ' -f1)" ]; then
    pass 'untouched products keep their bytes'
else
    fail 'untouched products keep their bytes' 'loader.elf or kerneldebug.sys moved'
fi
# The publish path runs on every delegation; the published set must track what
# the kernel checkout installed byte for byte.
synced=yes
for pair in system/kernel.sys:kernel.sys system/kernel.debug:kernel.debug \
            system/kerneldebug.sys:kerneldebug.sys boot/loader.elf:loader.elf; do
    rel=${pair%%:*}
    name=${pair#*:}
    cmp -s "$O/generated/$rel" "$O/kernel-install/$name" || synced=no
done
if [ "$synced" = yes ]; then
    pass 'published products are byte-identical to the kernel install output'
else
    fail 'published products are byte-identical to the kernel install output' \
        'a published file drifted from the install'
fi

printf '\n=== (c) a deleted published product is restored ===\n'
kernel_checksum=$(sha256sum "$O/generated/system/kernel.sys" | cut -d' ' -f1)
rm -f "$O/generated/system/kernel.sys" "$O/generated/system/kerneldebug.sys"
if build "$work/restore.log" "$O" &&
        [ -s "$O/generated/system/kernel.sys" ] && [ -s "$O/generated/system/kerneldebug.sys" ]; then
    pass 'the deleted kernel.sys and kerneldebug.sys are restored'
else
    fail 'the deleted kernel.sys and kerneldebug.sys are restored' 'still missing'
fi
if [ "$(sha256sum "$O/generated/system/kernel.sys" | cut -d' ' -f1)" = "$kernel_checksum" ]; then
    pass 'the restored kernel.sys is byte-identical'
else
    fail 'the restored kernel.sys is byte-identical' 'bytes differ'
fi

printf '\n=== (d) two parallel kernel builds on different output directories ===\n'
build "$work/par1.log" "$work/par1" &
par1_pid=$!
build "$work/par2.log" "$work/par2" &
par2_pid=$!
wait "$par1_pid"; par1_status=$?
wait "$par2_pid"; par2_status=$?
if [ "$par1_status" -eq 0 ] && [ "$par2_status" -eq 0 ]; then
    pass 'both parallel builds succeed'
else
    fail 'both parallel builds succeed' "statuses $par1_status $par2_status"
    tail -n 8 "$work/par1.log" "$work/par2.log" | sed 's/^/       | /'
fi
for tree in "$work/par1" "$work/par2"; do
    if [ -s "$tree/generated/system/kernel.sys" ] &&
            cmp -s "$tree/generated/system/kernel.sys" "$O/generated/system/kernel.sys"; then
        pass "$(basename "$tree") produces the same kernel image"
    else
        fail "$(basename "$tree") produces the same kernel image" 'missing or differs'
    fi
done

printf '\n=== (e) a build failure in the checkout publishes no half-products ===\n'
# Fixture: a scratch copy of the checkout. The real checkout is never edited.
fixture=$work/fixture
cp -a "$reliefnt" "$fixture" || exit 1
broken=$fixture/kernel/reliefnt/futex.c
fail_out=$work/fail-out

printf '\nthis is a deliberate syntax error in the fixture\n' >>"$broken"
if build "$work/fail.log" "$fail_out" RELIEFNT_DIR="$fixture"; then
    fail 'a syntax error in the checkout fails the parent build' 'make exited 0'
else
    pass 'a syntax error in the checkout fails the parent build'
fi
if grep -qF 'futex.c' "$work/fail.log"; then
    pass 'the sub-make compile error surfaces in the build output'
else
    fail 'the sub-make compile error surfaces in the build output' \
        "$(tail -c 300 "$work/fail.log")"
fi
if [ ! -e "$fail_out/generated/system/kernel.sys" ] &&
        [ ! -e "$fail_out/generated/boot/loader.elf" ] &&
        [ ! -e "$fail_out/generated/system/kerneldebug.sys" ]; then
    pass 'the failed build publishes no half-products'
else
    fail 'the failed build publishes no half-products' 'a product was published'
fi

# Recovery: fixing the source makes the same tree build and publish.
cp "$reliefnt/kernel/reliefnt/futex.c" "$broken"
if build "$work/recover.log" "$fail_out" RELIEFNT_DIR="$fixture" &&
        [ -s "$fail_out/generated/system/kernel.sys" ]; then
    pass 'the recovered fixture builds and publishes'
else
    fail 'the recovered fixture builds and publishes' 'make failed or no kernel.sys'
fi
recovered_checksum=$(sha256sum "$fail_out/generated/system/kernel.sys" | cut -d' ' -f1)

# A real content change must re-publish with new bytes (the dirty-modification
# path), and restoring the content must restore the image byte for byte.
printf '\nint reliefos_probe_marker = 1;\n' >>"$broken"
if build "$work/content.log" "$fail_out" RELIEFNT_DIR="$fixture"; then
    content_checksum=$(sha256sum "$fail_out/generated/system/kernel.sys" | cut -d' ' -f1)
    if [ "$content_checksum" != "$recovered_checksum" ]; then
        pass 'a content change in the checkout re-publishes a new kernel.sys'
    else
        fail 'a content change in the checkout re-publishes a new kernel.sys' \
            'kernel.sys bytes did not move'
    fi
else
    fail 'a content change in the checkout re-publishes a new kernel.sys' 'make failed'
fi
cp "$reliefnt/kernel/reliefnt/futex.c" "$broken"
if build "$work/restore-content.log" "$fail_out" RELIEFNT_DIR="$fixture" &&
        [ "$(sha256sum "$fail_out/generated/system/kernel.sys" | cut -d' ' -f1)" = \
          "$recovered_checksum" ]; then
    pass 'restoring the source restores the image byte-identical'
else
    fail 'restoring the source restores the image byte-identical' 'bytes differ'
fi

# Failure after products exist: the published set must stay the old complete
# set, not gain a half-published mix.
loader_checksum=$(sha256sum "$fail_out/generated/boot/loader.elf" | cut -d' ' -f1)
printf '\nthis is a deliberate syntax error in the fixture\n' >>"$broken"
rm -f "$fail_out/generated/system/kernel.sys"
if build "$work/fail2.log" "$fail_out" RELIEFNT_DIR="$fixture"; then
    fail 'a second failure keeps the parent build nonzero' 'make exited 0'
else
    pass 'a second failure keeps the parent build nonzero'
fi
if [ ! -e "$fail_out/generated/system/kernel.sys" ]; then
    pass 'a failed build does not restore a deleted product'
else
    fail 'a failed build does not restore a deleted product' 'kernel.sys reappeared'
fi
if [ "$(sha256sum "$fail_out/generated/boot/loader.elf" | cut -d' ' -f1)" = \
      "$loader_checksum" ]; then
    pass 'surviving products keep their bytes across a failed build'
else
    fail 'surviving products keep their bytes across a failed build' 'loader.elf moved'
fi

printf '\n%s: %d checks, %d failures\n' 'test-kernel-adapter' "$checks" "$failures"
[ "$failures" -eq 0 ] || exit 1
exit 0
