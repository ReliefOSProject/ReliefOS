#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RELIEFOS_SRC=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
THIRD_PARTY=$RELIEFOS_SRC/scripts/reliefos/Makefile.third-party
PAM_MAKEFILE=$RELIEFOS_SRC/scripts/reliefos/Makefile.pam
UPSTREAM_MAKEFILE=$RELIEFOS_SRC/scripts/reliefos/Makefile.upstream
UPSTREAM_MAKE_ENV=$RELIEFOS_SRC/tools/build/upstream-make-environment.sh
TMP_PARENT=${TMPDIR:-/tmp}
mkdir -p "$TMP_PARENT"
TEST_TMP=$(mktemp -d "$TMP_PARENT/reliefos-upstream-helper-deps.XXXXXX")
trap 'rm -rf "$TEST_TMP"' EXIT HUP INT TERM

if ! grep -Fq 'RELIEFOS_UPSTREAM_MAKE_ENV := $(RELIEFOS_SRC)/tools/build/upstream-make-environment.sh' "$THIRD_PARTY"; then
	printf 'not ok - producer helper variable does not name the shared upstream Make boundary\n' >&2
	exit 1
fi

check_normal_edge() {
	makefile=$1
	needle=$2
	label=$3
	if ! awk -v needle="$needle" '
		index($0, needle) && index($0, "&:") {
			count++
			split($0, side, "|")
			if (index(side[1], "RELIEFOS_UPSTREAM_MAKE_ENV"))
				found = 1
		}
		END { exit !(count == 1 && found) }
	' "$makefile"; then
		printf 'not ok - %s lacks the helper as a normal grouped-target prerequisite\n' "$label" >&2
		exit 1
	fi
	printf 'ok - %s depends normally on upstream Make environment helper\n' "$label"
}

check_normal_edge "$THIRD_PARTY" '$(MUSL_STAMP)' musl-sysroot
check_normal_edge "$THIRD_PARTY" '$(AUTH_STAMP)' auth-upstream
check_normal_edge "$PAM_MAKEFILE" '$(PAM_STAMP)' pam-stage
check_normal_edge "$UPSTREAM_MAKEFILE" 'upstream_$(1)_script' upstream-package
check_normal_edge "$UPSTREAM_MAKEFILE" '$(UPSTREAM_TERMINAL_SCRIPT)' upstream-terminal
check_normal_edge "$UPSTREAM_MAKEFILE" 'upstream_app_$(1)_inputs' upstream-app
check_normal_edge "$UPSTREAM_MAKEFILE" '$(BUSYBOX_ELF)' upstream-busybox

# Keep this real helper copy isolated: changing it proves that a normal Make
# prerequisite invalidates a completed producer without touching repository or
# product-tree timestamps.
cp "$UPSTREAM_MAKE_ENV" "$TEST_TMP/upstream-make-environment.sh"
touch -d '2000-01-01 00:00:00 UTC' "$TEST_TMP/upstream-make-environment.sh"
cat >"$TEST_TMP/Makefile" <<'EOF'
stage: upstream-make-environment.sh
	@touch -d '2001-01-01 00:00:00 UTC' "$@"
EOF
make --no-print-directory -s -C "$TEST_TMP" stage
if ! make --no-print-directory -q -C "$TEST_TMP" stage; then
	printf 'not ok - unchanged helper made its producer stale\n' >&2
	exit 1
fi
printf '\n# fixture helper edit\n' >>"$TEST_TMP/upstream-make-environment.sh"
touch -d '2002-01-01 00:00:00 UTC' "$TEST_TMP/upstream-make-environment.sh"
set +e
make --no-print-directory -q -C "$TEST_TMP" stage
status=$?
set -e
if [ "$status" -ne 1 ]; then
	printf 'not ok - helper change did not invalidate producer (make -q exit %s)\n' "$status" >&2
	exit 1
fi
printf 'ok - helper edit invalidates a completed producer\n'
