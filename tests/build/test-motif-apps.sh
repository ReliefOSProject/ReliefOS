#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-motif-apps.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
if [ ! -f "$src/userland/apps/calc/engine.c" ] || [ ! -f "$src/userland/apps/osver/debug_click.h" ]; then
    printf 'FAIL - calculator and system information have no toolkit-independent behavior boundary\n' >&2
    exit 1
fi
${HOSTCC:-cc} -std=c11 -Wall -Wextra -Werror -fsanitize=undefined -fno-sanitize-recover=all \
    -I"$src/userland/apps/calc" -I"$src/userland/apps/osver" \
    "$src/tests/host/test_motif_apps.c" "$src/userland/apps/calc/engine.c" \
    "$src/userland/apps/osver/debug_click.c" -o "$work/test"
"$work/test"

cat >"$work/sources.mk" <<'MAKE'
RELIEFOS_SRC := $(ROOT)
RELIEFOS_PASSIVE := 1
TARGET_CC := cc
TARGET_LD := ld
TARGET_CXX := c++
O := $(WORK)/out
include $(ROOT)/mk/userland.mk
.PHONY: sources
sources:
	@printf '%s\n' '$(call userland_sources,calc)' '$(call userland_sources,osver)' '$(call userland_sources,fileman)'
	@printf '%s\n' '$(USERLAND_LIBS_fileman)'
MAKE
make -s -f "$work/sources.mk" ROOT="$src" WORK="$work" sources >"$work/sources"
for app in calc osver fileman; do
    grep -q "userland/apps/$app/main.c" "$work/sources"
    for removed in native.c input.c view.c; do
        if grep -q "userland/apps/$app/$removed" "$work/sources"; then
            printf 'FAIL - %s still selects native frontend source %s\n' "$app" "$removed" >&2
            exit 1
        fi
    done
done
grep -q 'userland/apps/fileman/motif.c' "$work/sources"
grep -q 'userland/apps/fileman/motif_dialogs.c' "$work/sources"
grep -q 'libXm.so' "$work/sources"
printf 'ok - applications build only the Motif X11 frontend\n'
