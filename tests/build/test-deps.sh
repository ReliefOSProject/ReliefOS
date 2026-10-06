#!/bin/sh
# Contract tests for reliefos-deps, the reader of configs/dependencies.lock.json.
#
# The lock file decides which upstream source gets built and which digest is
# accepted, so these checks cover the tool's promises: what it validates, what
# it refuses, and what it prints for `make fetch`. The JSON reader itself is
# unit-tested in tests/host/test_json.c.
set -u

# The tool reports lock order as byte order; sort(1) collation must not depend
# on the caller's locale for the comparison to mean anything.
LC_ALL=C
export LC_ALL

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
cd "$repo_root" || exit 1

deps=${RELIEFOS_DEPS:-${LEONOS_DEPS:?set by mk/tests.mk}}
lock=${RELIEFOS_LOCK:-${LEONOS_LOCK:-configs/dependencies.lock.json}}

failures=0
checks=0
work=./reliefos-deps-contract.$$

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
    shift
    checks=$((checks + 1))
    if "$@" >/dev/null 2>&1; then
        printf 'FAIL - %s (expected a non-zero exit)\n' "$description"
        failures=$((failures + 1))
    else
        printf 'ok   - %s\n' "$description"
    fi
}

expect_output_is() {
    description=$1
    expected=$2
    shift 2
    checks=$((checks + 1))
    actual=$("$@" 2>/dev/null)
    if [ "$actual" = "$expected" ]; then
        printf 'ok   - %s\n' "$description"
    else
        printf 'FAIL - %s\n' "$description"
        printf '       expected: %s\n' "$expected"
        printf '       actual:   %s\n' "$actual"
        failures=$((failures + 1))
    fi
}

# entry <json> writes one dependency object into a minimal document so a single
# field can be varied per fixture.
entry_lock() {
    printf '{ "schema_version": 1, "dependencies": [ %s ] }\n' "$1" >"$work/lock.json"
    printf '%s' "$work/lock.json"
}

mkdir -p "$work" || exit 1
[ -x "$deps" ] || { printf 'FAIL - %s is missing; run make tools\n' "$deps" >&2; exit 1; }

# --- the shipped lock file ---------------------------------------------------
check 'the shipped lock file validates' "$deps" --lock "$lock" --check

ids=$("$deps" --lock "$lock" --list 2>/dev/null)
checks=$((checks + 1))
if [ -z "$ids" ]; then
    printf 'FAIL - --list must print at least one id\n'
    failures=$((failures + 1))
else
    printf 'ok   - --list prints ids\n'
fi
checks=$((checks + 1))
if printf '%s\n' "$ids" | grep -q '^musl$'; then
    printf 'ok   - --list contains the musl submodule\n'
else
    printf 'FAIL - --list must contain musl\n'
    failures=$((failures + 1))
fi
checks=$((checks + 1))
if [ "$(printf '%s\n' "$ids" | sort)" = "$ids" ]; then
    printf 'ok   - --list output is sorted\n'
else
    printf 'FAIL - --list output must be sorted\n'
    failures=$((failures + 1))
fi

# --- field queries ------------------------------------------------------------
expect_output_is 'a submodule reports its pinned commit' 9fa28ece75d8a2191de7c5bb53bed224c5947417 \
    "$deps" --lock "$lock" --id musl --print commit
expect_output_is 'a tarball reports its digest' \
    ac15ffb8430502fbaccdec66c5a82ee0eab0b0f36220df56710feadfeb13d0a0 \
    "$deps" --lock "$lock" --id libmd --print sha256
expect_output_is 'a tarball reports its download url' \
    https://libbsd.freedesktop.org/releases/libmd-1.2.0.tar.xz \
    "$deps" --lock "$lock" --id libmd --print url
expect_failure 'an unknown id fails rather than printing nothing' \
    "$deps" --lock "$lock" --id does-not-exist --print commit
expect_failure 'an unknown field fails rather than printing nothing' \
    "$deps" --lock "$lock" --id musl --print not-a-field
expect_failure 'querying a field the entry lacks fails' \
    "$deps" --lock "$lock" --id musl --print sha256

# --- the fetch list -----------------------------------------------------------
fetch_list=$("$deps" --lock "$lock" --fetch-list 2>/dev/null)
checks=$((checks + 1))
if printf '%s\n' "$fetch_list" | grep -q 'third_party/musl'; then
    printf 'FAIL - submodules must not appear in the fetch list\n'
    failures=$((failures + 1))
else
    printf 'ok   - submodules are excluded from the fetch list\n'
fi
checks=$((checks + 1))
if printf '%s\n' "$fetch_list" | grep -q 'libmd-1.2.0.tar.xz'; then
    printf 'ok   - downloaded tarballs appear in the fetch list\n'
else
    printf 'FAIL - downloaded tarballs must appear in the fetch list\n'
    failures=$((failures + 1))
fi
checks=$((checks + 1))
if printf '%s\n' "$fetch_list" | awk -F'\t' 'NF != 4 { bad = 1 } END { exit bad ? 1 : 0 }'; then
    printf 'ok   - every fetch line is digest, url and cache name\n'
else
    printf 'FAIL - every fetch line must be digest, url and cache name\n'
    failures=$((failures + 1))
fi
checks=$((checks + 1))
if printf '%s\n' "$fetch_list" | cut -f2 | grep -qE '^[0-9a-f]{64}$'; then
    printf 'ok   - fetch digests are lowercase hex\n'
else
    printf 'FAIL - fetch digests must be 64 lowercase hex digits\n'
    failures=$((failures + 1))
fi

# --- rejected lock files ------------------------------------------------------
D='0000000000000000000000000000000000000000000000000000000000000000'

expect_output_is 'xorg entry reports feature' xorg \
    "$deps" --lock "$lock" --id alpine-xdm --print feature
expect_output_is 'xterm entry reports pinned version' 410-r0 \
    "$deps" --lock "$lock" --id alpine-xterm --print version
expect_output_is 'xterm entry reports feature' xorg \
    "$deps" --lock "$lock" --id alpine-xterm --print feature
expect_output_is 'unmarked entry defaults to base' base \
    "$deps" --lock "$lock" --id alpine-openrc --print feature
expect_failure 'invalid feature is rejected' \
    "$deps" --lock "$(entry_lock '{"id":"a","kind":"apk","version":"1","url":"https://x/a.apk","sha256":"'$D'","directory":"a-1","license_in_source":".PKGINFO","feature":"desktop"}')" --check

expect_failure 'an entry missing required fields is rejected' \
    "$deps" --lock "$(entry_lock '{"id":"a","kind":"tarball"}')" --check

printf '%s\n' '{"schema_version":2,"dependencies":[]}' >"$work/v2.json"
expect_failure 'an unknown schema_version is rejected' "$deps" --lock "$work/v2.json" --check

expect_failure 'a tarball without a digest is rejected' \
    "$deps" --lock "$(entry_lock "{\"id\":\"a\",\"kind\":\"tarball\",\"version\":\"1\",\"url\":\"https://x/a.tgz\",\"directory\":\"a-1\",\"license_in_source\":\"COPYING\"}")" --check
expect_failure 'a short digest is rejected' \
    "$deps" --lock "$(entry_lock "{\"id\":\"a\",\"kind\":\"tarball\",\"version\":\"1\",\"url\":\"https://x/a.tgz\",\"sha256\":\"deadbeef\",\"directory\":\"a-1\",\"license_in_source\":\"COPYING\"}")" --check
expect_failure 'an uppercase digest is rejected' \
    "$deps" --lock "$(entry_lock "{\"id\":\"a\",\"kind\":\"tarball\",\"version\":\"1\",\"url\":\"https://x/a.tgz\",\"sha256\":\"AAAA000000000000000000000000000000000000000000000000000000000000\",\"directory\":\"a-1\",\"license_in_source\":\"COPYING\"}")" --check
expect_failure 'a downloaded entry without a version is rejected' \
    "$deps" --lock "$(entry_lock "{\"id\":\"a\",\"kind\":\"tarball\",\"url\":\"https://x/a.tgz\",\"sha256\":\"$D\",\"directory\":\"a-1\",\"license_in_source\":\"COPYING\"}")" --check
expect_failure 'an entry with no license record is rejected' \
    "$deps" --lock "$(entry_lock "{\"id\":\"a\",\"kind\":\"tarball\",\"version\":\"1\",\"url\":\"https://x/a.tgz\",\"sha256\":\"$D\",\"directory\":\"a-1\"}")" --check
expect_failure 'an unknown field is rejected rather than ignored' \
    "$deps" --lock "$(entry_lock "{\"id\":\"a\",\"kind\":\"tarball\",\"version\":\"1\",\"url\":\"https://x/a.tgz\",\"sha256\":\"$D\",\"directory\":\"a-1\",\"license_in_source\":\"COPYING\",\"build_me\":\"please\"}")" --check
expect_failure 'a submodule without a 40-hex commit is rejected' \
    "$deps" --lock "$(entry_lock '{"id":"a","kind":"submodule","commit":"deadbeef","directory":"third_party/a","license":"third_party/a/COPYING"}')" --check
expect_failure 'a non-HTTPS download url is rejected' \
    "$deps" --lock "$(entry_lock "{\"id\":\"a\",\"kind\":\"tarball\",\"version\":\"1\",\"url\":\"ftp://x/a.tgz\",\"sha256\":\"$D\",\"directory\":\"a-1\",\"license_in_source\":\"COPYING\"}")" --check
expect_failure 'a url with a query has no stable cache name and is rejected' \
    "$deps" --lock "$(entry_lock "{\"id\":\"a\",\"kind\":\"tarball\",\"version\":\"1\",\"url\":\"https://x/a.tgz?name=b\",\"sha256\":\"$D\",\"directory\":\"a-1\",\"license_in_source\":\"COPYING\"}")" --check
expect_failure 'a directory that escapes the tree is rejected' \
    "$deps" --lock "$(entry_lock "{\"id\":\"a\",\"kind\":\"tarball\",\"version\":\"1\",\"url\":\"https://x/a.tgz\",\"sha256\":\"$D\",\"directory\":\"../etc\",\"license_in_source\":\"COPYING\"}")" --check
expect_failure 'an absolute directory is rejected' \
    "$deps" --lock "$(entry_lock "{\"id\":\"a\",\"kind\":\"tarball\",\"version\":\"1\",\"url\":\"https://x/a.tgz\",\"sha256\":\"$D\",\"directory\":\"/etc\",\"license_in_source\":\"COPYING\"}")" --check
expect_failure 'duplicate ids are rejected' \
    "$deps" --lock "$(entry_lock "{\"id\":\"a\",\"kind\":\"tarball\",\"version\":\"1\",\"url\":\"https://x/a.tgz\",\"sha256\":\"$D\",\"directory\":\"a-1\",\"license_in_source\":\"COPYING\"},{\"id\":\"a\",\"kind\":\"tarball\",\"version\":\"2\",\"url\":\"https://x/b.tgz\",\"sha256\":\"$D\",\"directory\":\"b-1\",\"license_in_source\":\"COPYING\"}")" --check
expect_failure 'an unknown kind is rejected' \
    "$deps" --lock "$(entry_lock '{"id":"a","kind":"magic","version":"1"}')" --check
expect_failure 'a missing id is rejected' \
    "$deps" --lock "$(entry_lock "{\"kind\":\"tarball\",\"version\":\"1\",\"url\":\"https://x/a.tgz\",\"sha256\":\"$D\",\"directory\":\"a-1\",\"license_in_source\":\"COPYING\"}")" --check
expect_failure 'an id that is not filename safe is rejected' \
    "$deps" --lock "$(entry_lock "{\"id\":\"../a\",\"kind\":\"tarball\",\"version\":\"1\",\"url\":\"https://x/a.tgz\",\"sha256\":\"$D\",\"directory\":\"a-1\",\"license_in_source\":\"COPYING\"}")" --check
expect_failure 'extra top-level keys are rejected' \
    "$deps" --lock "$(printf '{ "schema_version": 1, "dependencies": [], "surprise": 1 }' | tee "$work/extra.json" >/dev/null; printf '%s' "$work/extra.json")" --check
printf '%s\n' 'not json at all' >"$work/garbage.json"
expect_failure 'garbage is rejected' "$deps" --lock "$work/garbage.json" --check
expect_failure 'a missing lock file is rejected' "$deps" --lock "$work/nope.json" --check

# --- patches and licenses on disk --------------------------------------------
mkdir -p "$work/tree/patches"
printf 'kept\n' >"$work/tree/patches/present.patch"

expect_failure 'a patch that is not an object is rejected' \
    "$deps" --lock "$(entry_lock "{\"id\":\"a\",\"kind\":\"tarball\",\"version\":\"1\",\"url\":\"https://x/a.tgz\",\"sha256\":\"$D\",\"directory\":\"a-1\",\"license_in_source\":\"COPYING\",\"patches\":[\"patches/present.patch\"]}")" --check
expect_failure 'a patch without a pinned digest is rejected' \
    "$deps" --lock "$(entry_lock "{\"id\":\"a\",\"kind\":\"tarball\",\"version\":\"1\",\"url\":\"https://x/a.tgz\",\"sha256\":\"$D\",\"directory\":\"a-1\",\"license_in_source\":\"COPYING\",\"patches\":[{\"path\":\"patches/present.patch\"}]}")" --check
expect_failure 'a patch listed but absent on disk is rejected' \
    "$deps" --lock "$(entry_lock "{\"id\":\"a\",\"kind\":\"tarball\",\"version\":\"1\",\"url\":\"https://x/a.tgz\",\"sha256\":\"$D\",\"directory\":\"a-1\",\"license_in_source\":\"COPYING\",\"patches\":[{\"path\":\"patches/absent.patch\",\"sha256\":\"$D\"}]}")" \
    --check --root "$work/tree"
expect_failure 'a license that is absent on disk is rejected' \
    "$deps" --lock "$(entry_lock "{\"id\":\"a\",\"kind\":\"tarball\",\"version\":\"1\",\"url\":\"https://x/a.tgz\",\"sha256\":\"$D\",\"directory\":\"a-1\",\"license\":\"LICENSE-absent\"}")" \
    --check --root "$work/tree"

printf '%s\n' "{\"schema_version\":1,\"dependencies\":[{\"id\":\"a\",\"kind\":\"tarball\",\"version\":\"1\",\"url\":\"https://x/a.tgz\",\"sha256\":\"$D\",\"directory\":\"a-1\",\"license_in_source\":\"COPYING\",\"patches\":[]}]}" >"$work/clean.json"
check 'a complete entry passes with --root' "$deps" --lock "$work/clean.json" --check --root "$work/tree"

# --- patch queries ------------------------------------------------------------
expect_output_is 'musl reports the digest of the patch it must apply' \
    'e336ed20b2d338dbbf88588dff043e8ce349d0d0044277d31be229d600bb93ad' \
    sh -c "$deps --lock $lock --id musl --list-patches | cut -f1"
expect_output_is 'and the path of that patch' \
    'patches/musl/0001-enforce-password-file-lock.patch' \
    sh -c "$deps --lock $lock --id musl --list-patches | cut -f2"
expect_output_is 'a dependency without patches prints nothing' '' \
    "$deps" --lock "$lock" --id zlib --list-patches
expect_failure '--list-patches without --id is rejected' \
    "$deps" --lock "$lock" --list-patches
expect_failure '--print without --id is rejected' \
    "$deps" --lock "$lock" --print commit

cleanup
printf -- '---\n'
if [ "$failures" -ne 0 ]; then
    printf 'not ok - reliefos-deps contract: %d of %d checks failed\n' "$failures" "$checks"
    exit 1
fi
printf 'ok - reliefos-deps contract: %d checks passed\n' "$checks"
