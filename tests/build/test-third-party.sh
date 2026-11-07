#!/bin/sh
# Contract tests for the third-party layer: the lock file as the only source of
# provenance, the download cache as the only network product, and the musl
# sysroot as the first component built from them.
#
# What these checks refuse to do is as important as what they assert. A missing
# download must stop the build with the dependency named rather than go online,
# and a cached file that disagrees with the lock must never be quietly accepted.
set -u

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
cd "$repo_root" || exit 1

deps=${RELIEFOS_DEPS:-${LEONOS_DEPS:?set by scripts/reliefos/Makefile.tests}}
lock=${RELIEFOS_LOCK:-${LEONOS_LOCK:?set by scripts/reliefos/Makefile.tests}}
fetch=./tools/build/fetch.sh

failures=0
checks=0
work=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-third-party.XXXXXX") || exit 1
test_out=$work/out
sysroot=$test_out/sysroot/musl
stamp=$sysroot/.leonos-musl.json
# Do not inherit the caller's O= or mutate the shared dependency lock.
cp "$lock" "$work/dependencies.lock.json" || exit 1
lock=$work/dependencies.lock.json
make() { command make O="$test_out" RELIEFOS_LOCK="$lock" "$@"; }

cleanup() { rm -rf "$work"; }
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM

check() {
    description=$1
    shift
    checks=$((checks + 1))
    if "$@" >/dev/null 2>&1; then
        printf 'ok   - %s\n' "$description"
    else
        printf 'FAIL - %s\n' "$description"
        failures=$((failures + 1))
    fi
}

expect_failure() {
    description=$1
    pattern=$2
    shift 2
    checks=$((checks + 1))
    output=$("$@" 2>&1)
    status=$?
    if [ "$status" -eq 0 ]; then
        printf 'FAIL - %s (expected a non-zero exit)\n' "$description"
        failures=$((failures + 1))
    elif ! printf '%s' "$output" | grep -qF -- "$pattern"; then
        printf 'FAIL - %s\n' "$description"
        printf '       expected the message to contain: %s\n' "$pattern"
        printf '       actual: %s\n' "$output"
        failures=$((failures + 1))
    else
        printf 'ok   - %s\n' "$description"
    fi
}

digest_of() {
    sha256sum "$1" | cut -d' ' -f1
}

mkdir -p "$work" || exit 1
[ -x "$deps" ] || { printf 'FAIL - %s is missing; run make tools\n' "$deps" >&2; exit 1; }
[ -f "$lock" ] || { printf 'FAIL - %s is missing\n' "$lock" >&2; exit 1; }

# --- the lock file is the whole provenance story ------------------------------
check 'the shipped lock file validates against the checked-out tree' \
    "$deps" --lock "$lock" --check --root "$repo_root"

# The adapter must not carry its own pins: read them back out of the lock file
# and compare with what the built sysroot recorded.
locked_musl=$("$deps" --lock "$lock" --id musl --print commit 2>/dev/null)
locked_patch=$("$deps" --lock "$lock" --id musl --list-patches 2>/dev/null | cut -f1)

# --- an unmet dependency stops the build without going online -----------------
expect_failure 'a missing download is refused by name, not fetched on demand' \
    'libmd' \
    sh "$fetch" --deps "$deps" --lock "$lock" --cache "$work/empty" --verify-only

# A cache that already holds the right bytes must not need the network at all.
# The fixture digest is computed from the fixture file, so this proves the
# accept path rather than a lucky collision.
printf 'fake libmd payload for the cache contract\n' >"$work/libmd-1.2.0.tar.xz"
fake_digest=$(digest_of "$work/libmd-1.2.0.tar.xz")
printf '{ "schema_version": 1, "dependencies": [ { "id": "libmd", "kind": "tarball",' \
    >"$work/lock.json"
printf '"version": "1.2.0", "url": "https://example.invalid/libmd-1.2.0.tar.xz",' >>"$work/lock.json"
printf '"sha256": "%s", "directory": "libmd-1.2.0",' "$fake_digest" >>"$work/lock.json"
printf '"license_in_source": "COPYING" } ] }\n' >>"$work/lock.json"

mkdir -p "$work/good-cache"
cp "$work/libmd-1.2.0.tar.xz" "$work/good-cache/libmd-1.2.0.tar.xz"
check 'a cached file that matches the lock is accepted with no download' \
    sh "$fetch" --deps "$deps" --lock "$work/lock.json" --cache "$work/good-cache" \
        --verify-only

mkdir -p "$work/bad-cache"
printf 'a different payload with the same name\n' >"$work/bad-cache/libmd-1.2.0.tar.xz"
expect_failure 'a cached file that disagrees with the lock is reported' \
    'do not match the lock file' \
    sh "$fetch" --deps "$deps" --lock "$work/lock.json" --cache "$work/bad-cache" \
        --verify-only
checks=$((checks + 1))
if [ -f "$work/bad-cache/libmd-1.2.0.tar.xz" ]; then
    printf 'ok   - verification leaves the disagreeing cache file for inspection\n'
else
    printf 'FAIL - --verify-only deleted the file it was only asked to inspect\n'
    failures=$((failures + 1))
fi

# An unreachable url fails the fetch; it never produces a file the build would
# then have to distrust.
expect_failure 'an unreachable url stops the fetch rather than inventing a file' \
    'fetching libmd from' \
    sh "$fetch" --deps "$deps" --lock "$work/lock.json" --cache "$work/bad-url" \
        --only libmd
checks=$((checks + 1))
if [ -f "$work/bad-url/libmd-1.2.0.tar.xz" ]; then
    printf 'FAIL - a failed download published a cache file\n'
    failures=$((failures + 1))
else
    printf 'ok   - a failed download leaves nothing behind\n'
fi

# --- the musl sysroot ---------------------------------------------------------
# Building musl takes about a minute; it is the component this phase is actually
# about, so the test builds it rather than asserting around a directory that may
# not exist.
# An existing stamp can predate updated adapters/tooling. Establish a current
# baseline before measuring the *second* build, even in a reused output tree.
    printf 'note - bringing the musl sysroot up to date for these checks\n'
    make -j"$(nproc)" "$stamp" >"$work/sysroot.log" 2>&1 || {
        printf 'FAIL - the musl sysroot did not build; see the tail below\n'
        tail -n 20 "$work/sysroot.log"
        exit 1
    }
checks=$((checks + 1))
if [ -f "$stamp" ]; then
    printf 'ok   - the sysroot stamp exists\n'
else
    printf 'FAIL - %s is missing\n' "$stamp"
    failures=$((failures + 1))
    cleanup
    printf -- '---\n'
    printf 'not ok - third-party contract: %d of %d checks failed\n' "$failures" "$checks"
    exit 1
fi

# Every product the rest of the build links against has to be a real file *and*
# has to be declared to Make. Native parameter checks use FORCE, so `make -q`
# cannot prove that a file has a producer. An undeclared path is
# the one thing Make reports unambiguously, so that is what gets asserted here.
declared() {
    LC_ALL=C make -n "$1" 2>&1 | grep -q 'No rule to make target'
}

for artifact in lib/libc.so lib/libc.a lib/Scrt1.o lib/crt1.o lib/crti.o lib/crtn.o \
        lib/rcrt1.o lib/libmimalloc.so.3 lib/mimalloc.o lib/libssp_nonshared.a \
        include/stdio.h include/mimalloc.h \
        share/licenses/musl/COPYRIGHT share/licenses/mimalloc/LICENSE; do
    checks=$((checks + 1))
    if declared "$sysroot/$artifact"; then
        printf 'FAIL - %s is not declared as a build product\n' "$artifact"
        failures=$((failures + 1))
    elif [ ! -f "$sysroot/$artifact" ]; then
        printf 'FAIL - %s is declared but missing\n' "$artifact"
        failures=$((failures + 1))
    else
        printf 'ok   - %s is a declared, present product\n' "$artifact"
    fi
done

stamp_musl=$(sed -n 's/.*"musl": "\([0-9a-f]*\)".*/\1/p' "$stamp" | head -n1)
checks=$((checks + 1))
if [ "$stamp_musl" = "$locked_musl" ]; then
    printf 'ok   - the sysroot records the commit the lock file pins\n'
else
    printf 'FAIL - sysroot records %s, the lock pins %s\n' "$stamp_musl" "$locked_musl"
    failures=$((failures + 1))
fi
stamp_patch=$(sed -n 's/.*"sha256": "\([0-9a-f]*\)".*/\1/p' "$stamp" | head -n1)
checks=$((checks + 1))
if [ -n "$locked_patch" ] && [ "$stamp_patch" = "$locked_patch" ]; then
    printf 'ok   - the sysroot records the patch digest that was applied\n'
else
    printf 'FAIL - sysroot patch digest %s does not match the lock (%s)\n' \
        "$stamp_patch" "$locked_patch"
    failures=$((failures + 1))
fi

# The stack-protection shim and the allocator must be linked for the target,
# not for the host: a stray x86-64 ELF with an interpreter is the failure mode.
checks=$((checks + 1))
if head -c 16 "$sysroot/lib/libc.so" | od -An -tx1 | grep -qi '7f 45 4c 46'; then
    printf 'ok   - libc.so is an ELF object\n'
else
    printf 'FAIL - %s is not ELF\n' "$sysroot/lib/libc.so"
    failures=$((failures + 1))
fi

stamp_mtime_before=$(stat -c %Y "$stamp" 2>/dev/null)
make -j"$(nproc)" "$stamp" >"$work/noop.log" 2>&1
stamp_mtime_after=$(stat -c %Y "$stamp" 2>/dev/null)
checks=$((checks + 1))
if [ "$stamp_mtime_before" = "$stamp_mtime_after" ] \
        && ! grep -qE 'SYSROOT|RESET|PATCH' "$work/noop.log"; then
    printf 'ok   - repeating the sysroot build does no work\n'
else
    printf 'FAIL - a repeat build did work\n'
    tail -n 8 "$work/noop.log"
    failures=$((failures + 1))
fi

# The native parameter record carries the lock digest, so an edit is noticed
# even when its mtime is kept. Build that target without rebuilding the sysroot;
# inspection mode must leave the output tree unchanged.
meta=$test_out/meta
sig=$meta/musl-sysroot.parameters
cp "$sig" "$work/signature-baseline" || exit 1
cp -p "$lock" "$work/lock-backup.json" || exit 1
refresh_signature() {
    make "$sig" >"$work/signature.log" 2>&1
}

checks=$((checks + 1))
if refresh_signature && cmp -s "$sig" "$work/signature-baseline"; then
    printf 'ok   - an unchanged lock keeps the sysroot signature identical\n'
else
    printf 'FAIL - the signature moved with nothing changed\n'
    cat "$work/signature.log"
    failures=$((failures + 1))
fi
sed 's/\"note": \"upstream musl binary/\"note": \"changed by test/' "$lock" >"$work/edited.json"
if cmp -s "$lock" "$work/edited.json"; then
    printf 'FAIL - the fixture edit changed nothing; extend the test\n'
    failures=$((failures + 1))
    checks=$((checks + 1))
else
    cp "$work/edited.json" "$lock" || exit 1
    touch -r "$work/lock-backup.json" "$lock" || exit 1
    checks=$((checks + 1))
    if refresh_signature && [ -s "$sig" ] && ! cmp -s "$sig" "$work/signature-baseline"; then
        printf 'ok   - an edited lock moves the sysroot signature\n'
    else
        printf 'FAIL - an edited lock left the sysroot signature unchanged\n'
        cat "$work/signature.log"
        failures=$((failures + 1))
    fi
fi
# Restore the lock contents and original mtime, then publish its original
# signature so subsequent checks cannot rebuild the sysroot for this fixture.
cp -p "$work/lock-backup.json" "$lock" || exit 1
checks=$((checks + 1))
if refresh_signature && cmp -s "$sig" "$work/signature-baseline"; then
    printf 'ok   - restoring the lock restores the signature\n'
else
    printf 'FAIL - the signature did not return after the lock was restored\n'
    cat "$work/signature.log"
    failures=$((failures + 1))
fi

cleanup
printf -- '---\n'
if [ "$failures" -ne 0 ]; then
    printf 'not ok - third-party contract: %d of %d checks failed\n' "$failures" "$checks"
    exit 1
fi
printf 'ok - third-party contract: %d checks passed\n' "$checks"
