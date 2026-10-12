#!/bin/sh
# Contract tests for the URL liveness check: tools/build/fetch-check.sh and the
# `make fetch-check` delegation into the ReliefNT checkout (mk/kernel.mk).
#
#   1. a locked URL that answers 2xx is reported OK and the check passes;
#   2. a locked URL that answers 404 fails the check, named by id and URL;
#   3. a mirror that refuses HEAD but serves GET is not called broken;
#   4. a host that does not answer at all fails with curl's exit code;
#   5. --only probes the named ids and nothing else;
#   6. `make fetch-check` runs the parent check and the kernel check together;
#   7. `make -n` never reaches the sub-make (the adapter's no-output promise);
#   8. a missing checkout warns and keeps the goal successful;
#   9. RELIEFNT_DIR pointing back at this repository skips instead of recursing.
#
# Everything is stubbed: curl is a PATH script answering per URL from a table,
# the kernel checkout is a fake Makefile, and the probe locks are fixtures --
# no network and no real cache is touched.
set -u
LC_ALL=C
export LC_ALL

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
cd "$repo_root" || exit 1

deps=${RELIEFOS_DEPS:-${LEONOS_DEPS:?set by mk/tests.mk}}
check_script=./tools/build/fetch-check.sh

work=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-fetch-check.XXXXXX") || exit 1
failures=0
checks=0

cleanup() { [ -n "${KEEP_WORK:-}" ] || rm -rf "$work"; }
trap cleanup EXIT INT TERM

pass() { checks=$((checks + 1)); printf 'ok   - %s\n' "$1"; }
fail() {
    checks=$((checks + 1))
    failures=$((failures + 1))
    printf 'FAIL - %s\n' "$1"
    if [ "$#" -gt 1 ]; then shift; printf '       %s\n' "$@"; fi
}

# The recursion case must not be able to hang the suite if the guard breaks.
if command -v timeout >/dev/null 2>&1; then
    guarded() { timeout 30 "$@"; }
else
    guarded() { "$@"; }
fi

# A curl stand-in that answers from a MODE URL table and defaults to HTTP 200.
# "die N" in the answer column stands for curl failing below HTTP with exit N.
mkdir -p "$work/bin"
cat >"$work/bin/curl" <<EOF
#!/bin/sh
mode=GET
url=
for arg in "\$@"; do
    case "\$arg" in
        --head) mode=HEAD ;;
        https://*|http://*) url=\$arg ;;
    esac
done
answer=\$(awk -v k="\$mode \$url" '\$1 " " \$2 == k { print \$3; exit }' '$work/answers' 2>/dev/null)
case "\$answer" in
    die*) exit "\${answer#die}" ;;
    '') printf '200' ;;
    *) printf '%s' "\$answer" ;;
esac
exit 0
EOF
chmod +x "$work/bin/curl"
: >"$work/answers"
PATH="$work/bin:$PATH"
export PATH

# fixture_lock FILE ID URL [ID URL]... -- the fetch-list schema subset that
# tools/build/fetch.sh fixtures use elsewhere in this suite directory.
fixture_lock() {
    fixture_file=$1
    shift
    fixture_digest=0000000000000000000000000000000000000000000000000000000000000000
    printf '{ "schema_version": 1, "dependencies": [' >"$fixture_file"
    fixture_sep=''
    while [ "$#" -gt 0 ]; do
        fixture_id=$1
        fixture_url=$2
        shift 2
        printf '%s{ "id": "%s", "kind": "tarball", "version": "1.0", ' \
            "$fixture_sep" "$fixture_id" >>"$fixture_file"
        printf '"url": "%s", "sha256": "%s", ' "$fixture_url" "$fixture_digest" >>"$fixture_file"
        printf '"directory": "%s-1.0", "license_in_source": "LICENSE" }' \
            "$fixture_id" >>"$fixture_file"
        fixture_sep=', '
    done
    printf ' ] }\n' >>"$fixture_file"
}

printf '=== (1) a URL that answers 2xx passes ===\n'
fixture_lock "$work/ok.lock" ok-pkg https://example.invalid/ok.apk
printf 'HEAD https://example.invalid/ok.apk 200\nGET https://example.invalid/ok.apk 200\n' \
    >>"$work/answers"
if sh "$check_script" --deps "$deps" --lock "$work/ok.lock" >"$work/ok.log" 2>&1; then
    pass 'a URL that answers 2xx passes the check'
else
    fail 'a URL that answers 2xx passes the check' "$(head -c 400 "$work/ok.log")"
fi
if grep -q 'ok-pkg (HTTP 200)' "$work/ok.log"; then
    pass 'the passing URL is reported by id with its status'
else
    fail 'the passing URL is reported by id with its status' "$(head -c 400 "$work/ok.log")"
fi

printf '\n=== (2) a URL that answers 404 fails by name ===\n'
fixture_lock "$work/missing.lock" missing-pkg https://example.invalid/missing.apk
printf 'HEAD https://example.invalid/missing.apk 404\nGET https://example.invalid/missing.apk 404\n' \
    >>"$work/answers"
if sh "$check_script" --deps "$deps" --lock "$work/missing.lock" >"$work/missing.log" 2>&1; then
    fail 'a 404 makes the check exit non-zero'
else
    pass 'a 404 makes the check exit non-zero'
fi
if grep -q 'missing-pkg (HTTP 404) https://example.invalid/missing.apk' "$work/missing.log"; then
    pass 'the failure names the id, the status and the URL'
else
    fail 'the failure names the id, the status and the URL' "$(head -c 400 "$work/missing.log")"
fi
if grep -q 'do not answer: missing-pkg' "$work/missing.log"; then
    pass 'the summary lists the broken id'
else
    fail 'the summary lists the broken id' "$(head -c 400 "$work/missing.log")"
fi

printf '\n=== (3) a mirror that refuses HEAD but serves GET is not broken ===\n'
fixture_lock "$work/headless.lock" headless-pkg https://example.invalid/headless.apk
printf 'HEAD https://example.invalid/headless.apk 405\nGET https://example.invalid/headless.apk 200\n' \
    >>"$work/answers"
if sh "$check_script" --deps "$deps" --lock "$work/headless.lock" >"$work/headless.log" 2>&1; then
    pass 'the ranged GET fallback accepts a HEAD-refusing mirror'
else
    fail 'the ranged GET fallback accepts a HEAD-refusing mirror' "$(head -c 400 "$work/headless.log")"
fi
if grep -q 'headless-pkg (HTTP 200)' "$work/headless.log"; then
    pass 'the fallback status is reported'
else
    fail 'the fallback status is reported' "$(head -c 400 "$work/headless.log")"
fi

printf '\n=== (4) a host that does not answer fails with curl exit code ===\n'
fixture_lock "$work/dead.lock" dead-pkg https://example.invalid/dead.apk
printf 'HEAD https://example.invalid/dead.apk die7\nGET https://example.invalid/dead.apk die7\n' \
    >>"$work/answers"
if sh "$check_script" --deps "$deps" --lock "$work/dead.lock" >"$work/dead.log" 2>&1; then
    fail 'an unanswered URL makes the check exit non-zero'
else
    pass 'an unanswered URL makes the check exit non-zero'
fi
if grep -q 'dead-pkg (curl exit 7)' "$work/dead.log"; then
    pass 'the failure reports curl exit code 7 (connection refused)'
else
    fail 'the failure reports curl exit code 7 (connection refused)' "$(head -c 400 "$work/dead.log")"
fi

printf '\n=== (5) --only probes the named ids and nothing else ===\n'
fixture_lock "$work/select.lock" \
    ok-pkg https://example.invalid/ok.apk \
    missing-pkg https://example.invalid/missing.apk
if sh "$check_script" --deps "$deps" --lock "$work/select.lock" --only ok-pkg \
        >"$work/select.log" 2>&1; then
    pass '--only with a passing id exits 0 despite a broken id in the lock'
else
    fail '--only with a passing id exits 0 despite a broken id in the lock' \
        "$(head -c 400 "$work/select.log")"
fi
if grep -q 'ok-pkg' "$work/select.log" && ! grep -q 'missing-pkg' "$work/select.log"; then
    pass 'only the selected id is probed'
else
    fail 'only the selected id is probed' "$(head -c 400 "$work/select.log")"
fi

printf '\n=== (6) make fetch-check runs the parent check and the kernel check ===\n'
mkdir -p "$work/reliefnt-stub"
cat >"$work/reliefnt-stub/Makefile" <<EOF
.PHONY: fetch-check
fetch-check:
	@echo called >>'$work/kernel-fetch-check-called'
EOF
rm -f "$work/kernel-fetch-check-called"
if make -s O="$work/out" LEONOS_DEPS_TOOL="$deps" \
        RELIEFNT_DIR="$work/reliefnt-stub" fetch-check >"$work/make.log" 2>&1; then
    pass 'make fetch-check exits 0'
else
    fail 'make fetch-check exits 0' "$(head -c 400 "$work/make.log")"
fi
if [ -f "$work/kernel-fetch-check-called" ]; then
    pass "the kernel checkout's fetch-check goal ran"
else
    fail "the kernel checkout's fetch-check goal ran" 'no call marker was written'
fi
if grep -q 'locked URLs answer' "$work/make.log"; then
    pass 'the parent URL check ran and summarized'
else
    fail 'the parent URL check ran and summarized' "$(head -c 400 "$work/make.log")"
fi

printf '\n=== (7) make -n never reaches the sub-make ===\n'
rm -f "$work/kernel-fetch-check-called"
if make -s -n O="$work/out" LEONOS_DEPS_TOOL="$deps" \
        RELIEFNT_DIR="$work/reliefnt-stub" reliefnt-fetch-check >"$work/dryrun.log" 2>&1; then
    pass 'make -n reliefnt-fetch-check exits 0'
else
    fail 'make -n reliefnt-fetch-check exits 0' "$(head -c 400 "$work/dryrun.log")"
fi
if [ ! -f "$work/kernel-fetch-check-called" ]; then
    pass 'make -n writes no call marker'
else
    fail 'make -n writes no call marker' 'the sub-make ran under -n'
fi

printf '\n=== (8) a missing checkout warns and stays successful ===\n'
rm -f "$work/kernel-fetch-check-called"
if make -s O="$work/out" LEONOS_DEPS_TOOL="$deps" \
        RELIEFNT_DIR="$work/nonexistent" reliefnt-fetch-check >"$work/missing-dir.log" 2>&1; then
    pass 'make reliefnt-fetch-check exits 0 without a checkout'
else
    fail 'make reliefnt-fetch-check exits 0 without a checkout' \
        "$(head -c 400 "$work/missing-dir.log")"
fi
if grep -q 'kernel checkout not found' "$work/missing-dir.log"; then
    pass 'the skip warns about the missing checkout'
else
    fail 'the skip warns about the missing checkout' "$(head -c 400 "$work/missing-dir.log")"
fi
if [ ! -f "$work/kernel-fetch-check-called" ]; then
    pass 'no check ran against a missing checkout'
else
    fail 'no check ran against a missing checkout' 'a call marker was written'
fi

printf '\n=== (9) RELIEFNT_DIR pointing at this repository skips instead of recursing ===\n'
rm -f "$work/kernel-fetch-check-called"
if guarded make -s O="$work/out" LEONOS_DEPS_TOOL="$deps" \
        RELIEFNT_DIR="$repo_root" reliefnt-fetch-check >"$work/self.log" 2>&1; then
    pass 'make reliefnt-fetch-check RELIEFNT_DIR=. exits 0 without recursing'
else
    fail 'make reliefnt-fetch-check RELIEFNT_DIR=. exits 0 without recursing' \
        "$(head -c 400 "$work/self.log")"
fi
if grep -q 'skipping' "$work/self.log"; then
    pass 'the self-pointing skip is announced'
else
    fail 'the self-pointing skip is announced' "$(head -c 400 "$work/self.log")"
fi

printf '\n%d checks, %d failures\n' "$checks" "$failures"
[ "$failures" -eq 0 ]
