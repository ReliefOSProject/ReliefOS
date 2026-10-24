#!/bin/sh
# Vendor Make adapters must restore upstream implicit rules without losing the
# parent Kbuild jobserver.
set -eu
repo=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd -P)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/fixture"

cat > "$work/fixture/Makefile" <<'MAKE'
BUILD_CC = cc
.PHONY: all jobserver
all: probe.o jobserver nested

# This mirrors e2fsprogs' explicit suffix recipe. With inherited -r, GNU make
# clears its suffix list and cannot use this .c.o rule until upstream flags are
# restored.
.c.o:
	@printf 'CC .c.o %s\n' "$<"
	$(BUILD_CC) -c -o "$@" "$<"

jobserver:
	@case "$$MAKEFLAGS" in *--jobserver-auth=*|*--jobserver-fds=*) : ;; *) echo 'upstream make lost the parent jobserver' >&2; exit 1 ;; esac

nested:
	+$(MAKE) --no-print-directory -f Nested.mk nested-check MARKER=nested.jobserver
MAKE

cat > "$work/fixture/Nested.mk" <<'MAKE'
.PHONY: nested-check
nested-check:
	@case "$$MAKEFLAGS" in *--jobserver-auth=*|*--jobserver-fds=*) : ;; *) echo 'nested make lost the parent jobserver' >&2; exit 1 ;; esac
	@printf 'jobserver preserved\n' > "$(MARKER)"
MAKE
cat > "$work/fixture/probe.c" <<'C'
int upstream_suffix_probe(void) { return 0; }
C

cat > "$work/run-upstream.sh" <<'SH'
#!/bin/sh
set -eu
repo=$1
fixture=$2
. "$repo/tools/build/upstream-make-environment.sh"
reliefos_upstream_make_environment
case ${MAKEFLAGS%% *} in
    *r*|*R*) echo 'upstream Make inherited native -r/-R flags' >&2; exit 1 ;;
    *k*) ;;
    *) echo 'upstream Make lost the caller keep-going flag' >&2; exit 1 ;;
esac
case $MAKEFLAGS in
    *--jobserver-auth=*|*--jobserver-fds=*) ;;
    *) echo 'upstream Make lost the caller jobserver flags' >&2; exit 1 ;;
esac
case ${MAKEFLAGS-} in
    *PARENT_SETTING=*) echo 'upstream Make inherited a parent override' >&2; exit 1 ;;
esac
[ -z "${MAKEOVERRIDES-}" ] || { echo 'upstream Make inherited MAKEOVERRIDES' >&2; exit 1; }
exec make --no-print-directory -C "$fixture" all
SH
chmod +x "$work/run-upstream.sh"

cat > "$work/outer.mk" <<'MAKE'
.PHONY: run
run:
	+@sh "$(RUNNER)" "$(REPO)" "$(FIXTURE)"
MAKE

env MAKEFLAGS= MAKEOVERRIDES= make --no-print-directory -k -rR -j4 \
    -f "$work/outer.mk" run REPO="$repo" RUNNER="$work/run-upstream.sh" \
    FIXTURE="$work/fixture" PARENT_SETTING=must-not-leak
test -s "$work/fixture/probe.o"
readelf -h "$work/fixture/probe.o" | grep -q 'Type:.*REL'
test "$(cat "$work/fixture/nested.jobserver")" = 'jobserver preserved'
printf 'upstream Make environment: real .c.o suffix build and recursive jobserver passed\n'
