#!/bin/sh
# Public entry-point contract tests for the GNU Make build system.
#
# These assert the command surface from the migration plan section 4 and the
# output-directory safety rules from section 6.3. They must pass on a host with
# nothing but a C compiler: no target toolchain, no fetched dependencies.
set -u

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
cd "$repo_root" || exit 1

failures=0
checks=0

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

expect_output_contains() {
    description=$1
    pattern=$2
    shift 2
    checks=$((checks + 1))
    if "$@" 2>&1 | grep -qF -- "$pattern"; then
        printf 'ok   - %s\n' "$description"
    else
        printf 'FAIL - %s\n' "$description"
        printf '       expected output to contain: %s\n' "$pattern"
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
    if [ "$status" -ne 0 ] && printf '%s' "$output" | grep -qF -- "$pattern"; then
        printf 'ok   - %s\n' "$description"
    else
        printf 'FAIL - %s\n' "$description"
        printf '       exit=%s, expected non-zero and text containing: %s\n' "$status" "$pattern"
        printf '%s\n' "$output" | sed 's/^/       | /'
        failures=$((failures + 1))
    fi
}

# --- help must work without a compiler and must not build anything ---------
check "default goal is help even with no compiler on PATH" \
    env PATH=/usr/bin:/bin CC=nonexistent-cc make -s V=1
expect_output_contains "help lists the documented targets" "installer" \
    make -s help
expect_output_contains "help lists the documented variables" "SOURCE_DATE_EPOCH" \
    make -s help
expect_output_contains "help documents the no-implicit-download rule" "fetch" \
    make -s help

check "help and clean leave no output tree behind" \
    sh -c 'make -s help >/dev/null 2>&1; make -s clean O=out/test-bootstrap >/dev/null 2>&1; test ! -e out/test-bootstrap/generated' \
    sh
check "help does not create a source-tree version header" \
    test ! -e include/generated/build_info.h

# --- HOSTCC and target CC are independent ---------------------------------
expect_output_contains "doctor reports the host compiler separately" "HOSTCC   ${HOSTCC:-cc}" \
    make -s doctor
# One variable per side: if doctor printed a single "compiler" line the assertion
# above would pass even when the host and target compilers were the same binary.
expect_output_contains "doctor names the target compiler and linker apart from it" \
    "TARGET_CC clang" make -s doctor
expect_output_contains "doctor reports the target linker" "TARGET_LD ld.lld" \
    make -s doctor
check "host tools build with a plain host compiler" \
    env HOSTCC=cc make -s tools
# doctor resolves the override before anything compiles, so a bogus CC is
# reported rather than discovered halfway through a link.
expect_failure "an explicit target CC override reaches doctor" "target-cc-probe" \
    env HOSTCC=cc make -s doctor CC=target-cc-probe

# --- rejected inputs ------------------------------------------------------
expect_failure "unknown ARCH is rejected" "ARCH" \
    make -s ARCH=riscv64 tools
expect_failure "unknown PROFILE is rejected" "PROFILE" \
    make -s PROFILE=turbo tools
expect_failure "empty O is rejected" "O=" \
    make -s O= tools
expect_failure "O=/ is rejected" "output directory" \
    make -s O=/ tools
expect_failure "O pointing at the source root is rejected" "output directory" \
    make -s O=. tools
expect_failure "O containing a space is rejected" "unsupported" \
    make -s O="out/bad dir" tools
expect_failure "O containing a newline is rejected" "unsupported" \
    make -s 'O=out/bad
dir' tools
expect_failure "unknown configuration key override is rejected" "CONFIG_BOGUS_KEY" \
    make -s CONFIG_BOGUS_KEY=1 tools
expect_failure "clean refuses an output dir without an ownership marker" "ownership" \
    make -s clean O=out/test-bootstrap

# --- doctor must fail loudly when the target toolchain is incomplete ------
expect_failure "doctor fails when the target linker is missing" "definitely-not-a-real-ld" \
    make -s doctor LD=definitely-not-a-real-ld
expect_failure "doctor fails when the target compiler cannot build for the triple" \
    "definitely-not-a-real-cc" make -s doctor CC=definitely-not-a-real-cc

printf '%s: %d checks, %d failures\n' "test-bootstrap" "$checks" "$failures"
[ "$failures" -eq 0 ] || exit 1
exit 0
