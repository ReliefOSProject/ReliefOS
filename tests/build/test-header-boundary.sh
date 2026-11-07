#!/bin/sh
# Header-boundary contract tests for the kernel/userland split.
#
# Asserts the rebuild scope the separation depends on:
#   1. a kernel-private header change must NOT recompile userland runtime;
#   2. a UAPI content change must reinstall the export and recompile its
#      userland consumers;
#   3. headers_install must not churn mtimes when nothing changed (otherwise
#      every build would silently rebuild all of userland);
#   4. the probe markers must leave the shared checkout byte-for-byte as they
#      were (a leaked marker dirties the tree for every later test).
set -u
LC_ALL=C
export LC_ALL

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
cd "$repo_root" || exit 1

# The kernel checkout under test (env-overridable, see test-incremental.sh).
reliefnt=${RELIEFNT_DIR:-${NTCLKS_DIR:-$repo_root/kernel/reliefnt}}

# Probe files live in the shared checkout, so runs must not interleave: two
# instances racing on the same files restore each other's half-written state
# into the tree and the loser's marker stays behind forever. The lock is keyed
# by checkout, and lives in its Git metadata so runs with different TMPDIR
# values still serialize without writing outside the repository's data disk.
probe_gitdir=$(git -C "$reliefnt" rev-parse --absolute-git-dir) || exit 1
probe_lock=$probe_gitdir/reliefos-header-boundary-probe-$(id -u).lock
exec 9>"$probe_lock" || exit 1
if ! flock -n 9; then
    printf 'FAIL - another header-boundary run on %s is in flight\n' "$reliefnt"
    exit 1
fi

probe_sched="$reliefnt/include/reliefnt/sched.h"
probe_fsabi="$reliefnt/include/uapi/reliefos/fs_abi.h"

# A run killed between marker append and restore leaves the marker behind for
# every later test to trip over (the release gate treats the checkout as dirty).
# Refuse to compound it: name the file and how to restore it.
for changed in "$probe_sched" "$probe_fsabi"; do
    if [ -f "$changed" ] && tail -n 1 "$changed" | grep -q 'header-boundary probe'; then
        printf 'FAIL - %s still carries a probe marker from an interrupted run\n' "$changed"
        printf '       restore it (git -C %s checkout -- <file>) and rerun\n' "$reliefnt"
        exit 1
    fi
done

work=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-header-boundary.XXXXXX") || exit 1
O="$work/out"
failures=0
checks=0

# Baselines: restored byte-for-byte at the end and checked. Backups stay in
# $work, never beside the probed sources: an in-tree .bak is itself dirt and
# races with any other writer of the same file. Restores copy without -p on
# purpose: the un-probe is a content change and Make must see it as one (a
# preserved old mtime hides the restore from the export's freshness checks and
# the change surfaces later as phantom churn).
backup_dir=$work/probe-backup
mkdir -p "$backup_dir" || exit 1
cp "$probe_sched" "$backup_dir/sched.h" || exit 1
cp "$probe_fsabi" "$backup_dir/fs_abi.h" || exit 1
probe_sched_sum=$(sha256sum "$probe_sched" | cut -d' ' -f1)
probe_fsabi_sum=$(sha256sum "$probe_fsabi" | cut -d' ' -f1)

cleanup() {
    # Restore the checkout first: tests append marker comments to live sources.
    [ -f "$backup_dir/sched.h" ] && cp "$backup_dir/sched.h" "$probe_sched"
    [ -f "$backup_dir/fs_abi.h" ] && cp "$backup_dir/fs_abi.h" "$probe_fsabi"
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

build() {
    # $1 = log file, rest = targets. Runtime only: it is the userland consumer
    # of the export and pulls musl/pam once per run.
    logfile=$1
    shift
    make -s O="$O" "$@" >"$logfile" 2>&1
    status=$?
    if [ "$status" -ne 0 ]; then
        printf 'FAIL - make exited with %s; assertions below mean nothing\n' "$status"
        tail -n 25 "$logfile" | sed 's/^/       | /'
        exit 1
    fi
}

cc_actions() { grep -cE '^  CC ' "$1" || true; }

export_include="$O/kernel-export/include"

printf '== header boundary ==\n'

# Baseline: config + export + runtime.
build "$work/0.log" defconfig headers_install runtime
[ -f "$export_include/reliefos/fs_abi.h" ] \
    && [ -f "$export_include/linux/types.h" ] \
    && pass "export tree installed under $export_include" \
    || fail "export tree missing" "$(find "$O/kernel-export" -type f | head)"

# 1. Kernel-private header change must not recompile the userland runtime.
printf '\n/* header-boundary probe */\n' >> "$probe_sched"
advance_clock
build "$work/1.log" runtime
if [ "$(cc_actions "$work/1.log")" -eq 0 ]; then
    pass "kernel-private header change recompiles 0 userland objects"
else
    fail "kernel-private header change recompiled userland objects:" \
        "$(grep -E '^  CC ' "$work/1.log" | head)"
fi
cp "$backup_dir/sched.h" "$probe_sched"

# 2. UAPI content change must reinstall the export and recompile consumers.
printf '\n/* header-boundary probe */\n' >> "$probe_fsabi"
advance_clock
build "$work/2.log" runtime
if cmp -s "$probe_fsabi" "$export_include/reliefos/fs_abi.h" &&
        [ "$(cc_actions "$work/2.log")" -ge 1 ]; then
    pass "UAPI content change reinstalled export and recompiled consumers"
else
    fail "UAPI content change did not propagate" \
        "export bytes must match the changed source; CC=$(cc_actions "$work/2.log")"
fi
cp "$backup_dir/fs_abi.h" "$probe_fsabi"

# 3. A forced reinstall with unchanged content must not touch installed files
#    (content-stable publishing; otherwise every build rebuilds userland).
advance_clock
build "$work/3.log" headers_install
before=$work/mtimes-before.txt
after=$work/mtimes-after.txt
find "$export_include" -type f -printf '%p %T@\n' | sort > "$before"
touch configs/header-export.list
advance_clock
build "$work/4.log" headers_install
find "$export_include" -type f -printf '%p %T@\n' | sort > "$after"
if cmp -s "$before" "$after"; then
    pass "unchanged export keeps installed mtimes stable"
else
    fail "installed mtimes churned without content change" \
        "$(diff "$before" "$after" | head)"
fi

# 5. The probes must be gone from the shared checkout: a leaked marker leaves
#    the tree dirty for every later test and for the release gate.
if [ "$(sha256sum "$probe_sched" | cut -d' ' -f1)" = "$probe_sched_sum" ] &&
   [ "$(sha256sum "$probe_fsabi" | cut -d' ' -f1)" = "$probe_fsabi_sum" ]; then
    pass "probe sources restored byte-for-byte"
else
    fail "probe sources restored byte-for-byte" \
        "sched.h:  $(sha256sum "$probe_sched" | cut -d' ' -f1) want $probe_sched_sum" \
        "fs_abi.h: $(sha256sum "$probe_fsabi" | cut -d' ' -f1) want $probe_fsabi_sum"
fi

printf '%s checks, %s failures\n' "$checks" "$failures"
[ "$failures" -eq 0 ]
