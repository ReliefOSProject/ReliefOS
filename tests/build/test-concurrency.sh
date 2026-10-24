#!/bin/sh
# Concurrency contract: acceptance row A09 of the migration plan, and the
# same-output-directory rule of section 6.3.
#
# The assertions are about outcomes a user can observe: who is allowed to write,
# what the refused build says, and whether the build that was allowed to run
# produced a complete, unharmed output tree. Nothing here trusts the lock file's
# mere existence.
set -u

LC_ALL=C
export LC_ALL

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
cd "$repo_root" || exit 1

work=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-concurrency.XXXXXX") || exit 1
failures=0
checks=0

cleanup() {
    if [ -z "${KEEP_WORK:-}" ]; then
        kill $held_pids 2>/dev/null
        rm -rf "$work"
    fi
}
trap cleanup EXIT INT TERM
held_pids=''

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

expect_ok() {
    if [ "$1" -eq 0 ]; then pass "$2"; else fail "$2" "exit status $1"; fi
}

# A build that starts `hold` seconds before `contend` on the same output
# directory. The older make is the one that must win, so the two roles are never
# interchangeable in the assertions below.
same_o="$work/same"
other_a="$work/different-a"
other_b="$work/different-b"

printf '=== A09: two top-level makes on one output directory ===\n'
make -s O="$same_o" kernel >"$work/owner.log" 2>&1 &
owner_pid=$!
held_pids="$held_pids $owner_pid"
sleep 3

make -s O="$same_o" kernel >"$work/contender.log" 2>&1 &
contender_pid=$!
held_pids="$held_pids $contender_pid"

# Goals that write nothing must stay available while the lock is held.
make -s O="$same_o" -n kernel >"$work/dryrun.log" 2>&1
dryrun_status=$?
make -s O="$same_o" help >"$work/help.log" 2>&1
help_status=$?
make -s O="$same_o" doctor >"$work/doctor.log" 2>&1
doctor_status=$?

wait "$owner_pid"; owner_status=$?
wait "$contender_pid"; contender_status=$?

if [ "$owner_status" -eq 0 ]; then
    pass 'the older make completes the build'
else
    fail 'the older make completes the build' "exit status $owner_status"
    tail -n 15 "$work/owner.log" | sed 's/^/       | /'
fi

if [ "$contender_status" -ne 0 ]; then
    pass 'the newer make on the same output directory is refused'
else
    fail 'the newer make on the same output directory is refused' \
        'it exited 0, so both processes wrote the same tree'
fi
if grep -q 'refusing to build' "$work/contender.log"; then
    pass 'the refused build names its owner and says what to do'
    grep -E 'owner:|output directory' "$work/contender.log" | sed 's/^/       | /'
else
    fail 'the refused build names its owner and says what to do' \
        "$(head -c 200 "$work/contender.log")"
fi

# The refusal must not leave a half-written tree behind, and the winner must not
# have been disturbed by the loser parsing the same makefiles.
archive="$same_o/reliefnt/built-in.a"
llvm-ar t "$archive" > "$work/archive-members"
listed=$(wc -l < "$work/archive-members")
owner_objects=0
while IFS= read -r member; do
    [ ! -s "$member" ] || owner_objects=$((owner_objects + 1))
done < "$work/archive-members"
if [ "$owner_objects" = "$listed" ] && [ "$owner_objects" -gt 50 ]; then
    pass "the winning build has every manifest source as an object ($owner_objects)"
else
    fail 'the winning build has every manifest source as an object' \
        "objects=$owner_objects manifest=$listed"
fi
if [ -s "$same_o/generated/system/kernel.sys" ]; then
    pass 'the winning build leaves a non-empty kernel image'
else
    fail 'the winning build leaves a non-empty kernel image' 'kernel.sys missing or empty'
fi

expect_ok "$dryrun_status" 'make -n is exempt from the lock'
expect_ok "$help_status" 'make help is exempt from the lock'
expect_ok "$doctor_status" 'make doctor is exempt from the lock'

printf '\n=== A09: the finished build releases the directory ===\n'
# There is no Make exit hook, so a record outlives its process on purpose and
# liveness is what releases it. The next build must prune it, not refuse.
make -s O="$same_o" kernel >"$work/after.log" 2>&1
after_status=$?
expect_ok "$after_status" 'the next make on the same directory runs once the owner is gone'
if grep -q 'refusing to build' "$work/after.log"; then
    fail 'the next make must not see a stale record' 'it refused instead'
else
    pass 'the next make must not see a stale record'
fi

printf '\n=== A09: two different output directories build concurrently ===\n'
make -s O="$other_a" kernel >"$work/a.log" 2>&1 &
a_pid=$!
held_pids="$held_pids $a_pid"
make -s O="$other_b" kernel >"$work/b.log" 2>&1 &
b_pid=$!
held_pids="$held_pids $b_pid"
wait "$a_pid"; a_status=$?
wait "$b_pid"; b_status=$?
expect_ok "$a_status" 'the first output directory builds'
expect_ok "$b_status" 'the second output directory builds'
for tree in "$other_a" "$other_b"; do
    name=$(basename "$tree")
    if [ -s "$tree/generated/system/kernel.sys" ]; then
        pass "$name has its own kernel image"
    else
        fail "$name has its own kernel image" 'missing or empty'
    fi
    if grep -qF -- "$tree" "$tree/reliefnt/arch/x86_64/.apic.o.cmd" 2>/dev/null; then
        pass "$name's native command record points at $name"
    else
        fail "$name's native command record points at $name" \
            'the native command references another tree: outputs were crossed'
    fi
done

printf '\n=== A09: recursive make inherits the lock instead of re-acquiring ===\n'
# defconfig recurses into $(MAKE) for the generated config; if a nested make
# re-acquired, this would refuse itself and deadlock or fail.
make -s O="$other_a" defconfig >"$work/defconfig.log" 2>&1
nested_status=$?
expect_ok "$nested_status" 'a nested make does not refuse its own outer build'
if grep -q 'refusing to build' "$work/defconfig.log"; then
    fail 'a nested make does not refuse its own outer build' \
        "$(head -c 200 "$work/defconfig.log")"
fi

printf '\n%s: %d checks, %d failures\n' 'test-concurrency' "$checks" "$failures"
[ "$failures" -eq 0 ] || exit 1
exit 0
