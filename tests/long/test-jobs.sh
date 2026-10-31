#!/bin/sh
# Parallel-schedule contract: acceptance rows A08 and the interrupt half of A10.
#
# "-j1 and -j8, repeated three times: no races, no missing files, comparable
# unpacked content identical". The measurable reading of that: the *same work*
# has to be selected at both job levels, and what comes out has to be the same
# bytes. A build that merely succeeds twice at each level would also pass if one
# level had rebuilt 83 objects where the other rebuilt 82.
set -u

LC_ALL=C
export LC_ALL

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
cd "$repo_root" || exit 1

# The kernel checkout under test (env-overridable, see tests/build/test-incremental.sh).
source_kernel=${RELIEFNT_DIR:-${NTCLKS_DIR:-$repo_root/kernel/reliefnt}}

work=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-jobs.XXXXXX") || exit 1
# Invalidations and interruption must not touch the shared source checkout.
reliefnt=$work/kernel
python3 - "$source_kernel" "$reliefnt" <<'PYCOPY'
from pathlib import Path
import shutil, sys
source, destination = map(Path, sys.argv[1:])
def ignored(directory, names):
    excluded = {'__pycache__'}
    if Path(directory) == source:
        excluded.update(('.git', 'out', 'cache'))
    return set(names) & excluded
shutil.copytree(source, destination, ignore=ignored)
shutil.copytree(source / 'cache', destination / 'cache')
PYCOPY
failures=0
checks=0

cleanup() {
    [ -n "${KEEP_WORK:-}" ] || rm -rf "$work"
}
trap cleanup EXIT INT TERM

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

# Fixed inputs on both sides: the epoch removes the wall clock,
# so the two trees are byte-comparable by construction and any
# difference left over belongs to the scheduler.
epoch=1700000000
jobs="1 8"
rounds=3

# Paths are recorded relative to their own tree: the two output directories have
# different names, and a comparison that includes them measures the test fixture
# rather than the work Make chose to do.
snapshot() {
    find "$1/reliefnt" -name '*.o' -printf '%P %T@\n' 2>/dev/null | LC_ALL=C sort
}

changed_between() {
    awk 'NR==FNR { seen[$1] = $2; next }
                ($1 in seen) && seen[$1] != $2 { print $1 }' "$1" "$2" | LC_ALL=C sort
}

# The three invalidations are deliberately different action classes: one ordinary
# source, one header with dozens of consumers, and the linker script, which must
# relink without recompiling anything. They live in the kernel checkout.
touch_targets="$reliefnt/kernel/ipc/futex.c $reliefnt/include/reliefnt/types.h
$reliefnt/arch/x86_64/linker.ld"

printf '=== A08: -j1 and -j8 from clean output directories ===\n'
for level in $jobs; do
    tree=$work/j$level
    make RELIEFNT_DIR="$reliefnt" -s O="$tree" -j"$level" SOURCE_DATE_EPOCH=$epoch \
        kernel >"$work/clean-j$level.log" 2>&1
    status=$?
    if [ "$status" -eq 0 ] && [ -s "$tree/generated/system/kernel.sys" ]; then
        pass "-j$level clean build succeeds and leaves an image"
    else
        fail "-j$level clean build succeeds and leaves an image" \
            "exit status $status"
        tail -n 12 "$work/clean-j$level.log" | sed 's/^/       | /'
    fi
    if grep -qE "No rule to make target|Error [0-9]+|cannot be read|does not exist" \
            "$work/clean-j$level.log"; then
        fail "-j$level clean build reports no missing-file or rule error" \
            "$(grep -m2 -E 'No rule to make target|Error [0-9]+' "$work/clean-j$level.log")"
    else
        pass "-j$level clean build reports no missing-file or rule error"
    fi
    # Native records identify the exact actions rather than broad action-class
    # signatures. Normalise O when comparing the same build at two job levels.
    missing_record=''
    for record in arch/x86_64/interrupt/.apic.o.cmd arch/x86_64/boot/.boot.o.cmd generated/system/.kernel.unstripped.cmd generated/system/.kernel.sys.cmd; do
        [ -s "$tree/reliefnt/$record" ] || missing_record="$missing_record $record"
    done
    if [ -z "$missing_record" ]; then
        pass "-j$level publishes native records for C, ASM, link and objcopy"
    else
        fail "-j$level publishes native action records" "missing:$missing_record"
    fi
    sed "s#$tree#<O>#g" "$tree/reliefnt/arch/x86_64/interrupt/.apic.o.cmd" >"$work/command-j$level" 2>/dev/null || : >"$work/command-j$level"

done

if [ -s "$work/command-j1" ] && [ -s "$work/command-j8" ]; then
    if cmp -s "$work/command-j1" "$work/command-j8"; then
        pass 'both job levels compiled with the same tool, identity and flags'
    else
        fail 'both job levels compiled with the same tool, identity and flags'
        diff -u "$work/command-j1" "$work/command-j8" | tail -n 6 | sed 's/^/       | /'
    fi
else
    fail 'both job levels produced a native C command record to compare'
fi

printf '\n=== A08: the same invalidations at both job levels, three rounds ===\n'
# Round 1 is the heavy one (the header 82 objects consume) so a full-scale
# rebuild is compared once at each level; rounds 2 and 3 repeat the cheap pair
# because a race is nondeterministic and only repetition shows the choice of work
# is stable. The cheap pair is also two different action classes: one object, and
# a relink with no recompilation at all.
heavy_targets=$touch_targets
light_targets="$reliefnt/kernel/ipc/futex.c $reliefnt/arch/x86_64/linker.ld"
for round in 1 2 3; do
    if [ "$round" = 1 ]; then
        round_targets=$heavy_targets
    else
        round_targets=$light_targets
    fi
    # One invalidation per round, applied to both trees before either is rebuilt.
    # Touching inside the per-level loop would make the second level see a header
    # newer than the first level's freshly compiled objects, and the comparison
    # would then measure the test's own ordering instead of Make's.
    for level in $jobs; do
        snapshot "$work/j$level" >"$work/before-j$level-$round"
    done
    sleep 1
    for target in $round_targets; do
        touch "$target"
    done
    for level in $jobs; do
        tree=$work/j$level
        make RELIEFNT_DIR="$reliefnt" -s O="$tree" -j"$level" SOURCE_DATE_EPOCH=$epoch \
            kernel >"$work/round-j$level-$round.log" 2>&1
        status=$?
        snapshot "$tree" >"$work/after-j$level-$round"
        changed_between "$work/before-j$level-$round" "$work/after-j$level-$round" \
            >"$work/moved-j$level-$round"
        if [ "$status" -ne 0 ]; then
            fail "-j$level round $round exits zero" "status $status"
            tail -n 12 "$work/round-j$level-$round.log" | sed 's/^/       | /'
        fi
        if [ "$(grep -c '' "$work/moved-j$level-$round")" -eq 0 ]; then
            fail "-j$level round $round rebuilds something" 'no object changed mtime'
        fi
    done
    if [ -s "$work/moved-j1-$round" ] && cmp -s "$work/moved-j1-$round" \
            "$work/moved-j8-$round"; then
        pass "round $round selects the identical object set at -j1 and -j8 ($(grep -c '' "$work/moved-j1-$round") objects)"
    else
        diff -u "$work/moved-j1-$round" "$work/moved-j8-$round" |
            head -n 12 >"$work/diff-$round"
        fail "round $round selects the identical object set at -j1 and -j8" \
            "-j1=$(grep -c '' "$work/moved-j1-$round") -j8=$(grep -c '' "$work/moved-j8-$round")"
        sed 's/^/       | /' "$work/diff-$round"
    fi
done

printf '\n=== A08: comparable output content ===\n'
for product in generated/system/kernel.sys generated/system/kernel.debug \
        reliefnt/generated/system/kernel.unstripped reliefnt/built-in.a \
        include/generated/autoconf.h reliefnt/include/generated/build_info.h; do
    a=$work/j1/$product
    b=$work/j8/$product
    if [ ! -f "$a" ] || [ ! -f "$b" ]; then
        fail "$product exists in both trees"
        continue
    fi
    ha=$(sha256sum "$a" | awk '{ print substr($1, 1, 12) }')
    hb=$(sha256sum "$b" | awk '{ print substr($1, 1, 12) }')
    if [ "$ha" = "$hb" ]; then
        pass "$product is identical at -j1 and -j8 ($ha...)"
    else
        fail "$product is identical at -j1 and -j8" "j1=$ha... j8=$hb..."
    fi
done

printf '\n=== A10: an interrupted parallel build leaves no false product ===\n'
# .DELETE_ON_ERROR is only a claim until something is interrupted. -j2 with 82
# objects to recompile gives the section room to land the signal in the middle of
# real work, which the first assertion then proves rather than assumes.
victim=$work/interrupt
make RELIEFNT_DIR="$reliefnt" -s O="$victim" -j8 SOURCE_DATE_EPOCH=$epoch kernel \
        >"$work/interrupt-base.log" 2>&1
good=$work/interrupt-image
cp "$victim/generated/system/kernel.sys" "$good"
snapshot "$victim" >"$work/before-interrupt"
rm -f "$victim/generated/system/kernel.sys"
for target in $touch_targets; do
    touch "$target"
done
# A fixed sleep is a guess: this tree recompiles 82 objects in a few seconds on a
# fast machine, and an interrupt that arrives after the link proves nothing. Wait
# until the build has demonstrably started rewriting objects, then signal it.
# SIGTERM, not SIGINT: a POSIX shell with job control disabled sets SIGINT to
# ignore on background commands, so `kill -INT` here would be swallowed and the
# build would run to completion (observed: status 0 with all 82 objects rebuilt).
# Make handles both signals through the same cleanup path that honours
# .DELETE_ON_ERROR; the interactive Ctrl-C case needs a pty and is not covered.
: >"$work/interrupt-trigger"
make RELIEFNT_DIR="$reliefnt" -s O="$victim" -j2 SOURCE_DATE_EPOCH=$epoch kernel \
        >"$work/interrupt.log" 2>&1 &
victim_pid=$!
waited=0
while [ "$waited" -lt 200 ]; do
    if [ -n "$(find "$victim/reliefnt/kernel" -name '*.o' -newer "$work/interrupt-trigger" \
            -print -quit 2>/dev/null)" ]; then
        break
    fi
    kill -0 "$victim_pid" 2>/dev/null || break
    sleep 0.05
    waited=$((waited + 1))
done
kill -TERM "$victim_pid" 2>/dev/null
wait "$victim_pid" 2>/dev/null
interrupt_status=$?
snapshot "$victim" >"$work/after-interrupt"
moved_during=$(changed_between "$work/before-interrupt" "$work/after-interrupt" |
        grep -c '' || true)
if [ "$interrupt_status" -ne 0 ] && [ "$moved_during" -gt 0 ]; then
    pass "the interrupt landed mid-work (status $interrupt_status, $moved_during objects rewritten)"
else
    fail 'the interrupt landed mid-work' \
        "status=$interrupt_status objects rewritten while interrupted=$moved_during"
fi
if [ -e "$victim/generated/system/kernel.sys" ] &&
        ! cmp -s "$victim/generated/system/kernel.sys" "$good"; then
    fail 'an interrupted build does not leave a partial image claimed as current' \
        'kernel.sys exists and differs from the last complete one'
else
    pass 'an interrupted build does not leave a partial image claimed as current'
fi
# The rerun has to recover on its own.
make RELIEFNT_DIR="$reliefnt" -s O="$victim" -j8 SOURCE_DATE_EPOCH=$epoch kernel \
        >"$work/recover.log" 2>&1
status=$?
if [ "$status" -eq 0 ] && cmp -s "$victim/generated/system/kernel.sys" "$good"; then
    pass 'the rerun after an interrupt restores the exact image'
else
    fail 'the rerun after an interrupt restores the exact image' \
        "status $status, image $(cmp -s "$victim/generated/system/kernel.sys" "$good" && echo same || echo differs)"
    tail -n 10 "$work/recover.log" | sed 's/^/       | /'
fi

# Whatever the interruption left behind, the tracked sources must be intact.
for target in $touch_targets; do
    [ -f "$target" ] || fail 'interrupted build leaves the tracked sources intact' "$target missing"
done

printf '\n%s: %d checks, %d failures\n' 'test-jobs' "$checks" "$failures"
[ "$failures" -eq 0 ] || exit 1
exit 0
