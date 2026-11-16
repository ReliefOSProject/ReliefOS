#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
headers=${RELIEFOS_TEST_HEADERS:-$src/out/ci-validation/kernel-export/include}
test -f "$headers/reliefos/prctl.h" || {
    echo 'Set RELIEFOS_TEST_HEADERS to the headers_install include directory' >&2; exit 1;
}
${HOSTCC:-cc} -std=c11 -O2 -Wall -Wextra -Werror -idirafter "$headers" \
    "$src/userland/apps/linuxuname/main.c" "$src/tools/tests/linuxuname_prctl_stub.c" \
    -o "$work/linuxuname"
run_status() {
    expected=$1; shift
    if "$@" >"$work/stdout" 2>"$work/stderr"; then actual=0; else actual=$?; fi
    test "$actual" -eq "$expected"
}
run_status 0 "$work/linuxuname" --help
run_status 125 "$work/linuxuname"
run_status 125 "$work/linuxuname" --
run_status 127 "$work/linuxuname" "$work/missing-command"
printf '#!/bin/sh\nprintf "%%s\\n" "$PWD" "$LINUXUNAME_TEST" "$@"\nexit 37\n' > "$work/target program"
run_status 126 "$work/linuxuname" "$work/target program"
chmod +x "$work/target program"
cd "$work"
PATH="$work:$PATH" LINUXUNAME_TEST=present run_status 37 "$work/linuxuname" \
    'target program' --child-option 'argument with spaces' '' 'literal $(no-shell-expansion)'
printf '%s\n' "$work" present --child-option 'argument with spaces' '' \
    'literal $(no-shell-expansion)' > "$work/expected"
cmp "$work/expected" "$work/stdout"
LINUXUNAME_TEST=present run_status 37 "$work/linuxuname" -- './target program' after-separator
printf '%s\n' "$work" present after-separator > "$work/expected"
cmp "$work/expected" "$work/stdout"
LINUXUNAME_PRCTL_FAIL=1 run_status 125 "$work/linuxuname" './target program'
test ! -s "$work/stdout"
grep -q 'cannot select Linux uname' "$work/stderr"
printf 'linuxuname command: PATH, arguments, environment, cwd, exec statuses and prctl failure passed\n'
