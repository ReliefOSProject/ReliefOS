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
	@printf '%s\n' '$(call userland_sources,calc)' '$(call userland_sources,osver)'
MAKE
for backend in n y; do
    make -s -f "$work/sources.mk" ROOT="$src" WORK="$work" \
        KCONFIG_CONFIG_DESKTOP_BACKEND_XORG="$backend" sources >"$work/sources"
    if [ "$backend" = y ]; then selected=main; excluded=native; else selected=native; excluded=main; fi
    for app in calc osver; do
        grep -q "userland/apps/$app/$selected.c" "$work/sources"
        if grep -q "userland/apps/$app/$excluded.c" "$work/sources"; then
            printf 'FAIL - %s includes both desktop frontends\n' "$app" >&2
            exit 1
        fi
    done
done
printf 'ok - native and Xorg builds select exactly one frontend per application\n'
