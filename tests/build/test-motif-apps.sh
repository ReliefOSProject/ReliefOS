#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-motif-apps.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
if [ ! -f "$src/userland/apps/calc/engine.c" ] || [ ! -f "$src/userland/apps/osver/debug_click.h" ]; then
    printf 'FAIL - calculator and system information have no toolkit-independent behavior boundary\n' >&2
    exit 1
fi
for app in taskmgr minesweeper leonmmcoset xiaobai; do
    if [ ! -f "$src/userland/apps/$app/model.c" ] || [ ! -f "$src/userland/apps/$app/model.h" ]; then
        printf 'FAIL - %s has no toolkit-independent behavior boundary\n' "$app" >&2
        exit 1
    fi
done
${HOSTCC:-cc} -std=c11 -Wall -Wextra -Werror -fsanitize=undefined -fno-sanitize-recover=all \
    -I"$src/userland/apps" -I"$src/userland/apps/calc" -I"$src/userland/apps/osver" \
    -I"$src/userland/apps/taskmgr" \
    "$src/tests/host/test_motif_apps.c" "$src/userland/apps/calc/engine.c" \
    "$src/userland/apps/osver/debug_click.c" "$src/userland/apps/taskmgr/model.c" \
    "$src/userland/apps/minesweeper/model.c" "$src/userland/apps/leonmmcoset/model.c" \
    "$src/userland/apps/xiaobai/model.c" -o "$work/test"
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
	@printf '%s\n' '$(call userland_sources,calc)' '$(call userland_sources,osver)' '$(call userland_sources,fileman)' '$(call userland_sources,taskmgr)' '$(call userland_sources,minesweeper)' '$(call userland_sources,leonmmcoset)' '$(call userland_sources,xiaobai)'
	@printf '%s\n' '$(USERLAND_LIBS_fileman)' '$(USERLAND_LIBS_taskmgr)' '$(USERLAND_LIBS_minesweeper)' '$(USERLAND_LIBS_leonmmcoset)' '$(USERLAND_LIBS_xiaobai)'
MAKE
make -s -f "$work/sources.mk" ROOT="$src" WORK="$work" sources >"$work/sources"
for app in calc osver fileman taskmgr minesweeper leonmmcoset xiaobai; do
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
for app in taskmgr minesweeper leonmmcoset xiaobai; do
    grep -q "userland/apps/$app/model.c" "$work/sources"
    if grep -q 'reliefos_gui_create_app_window\|reliefos_ui_' \
        "$src/userland/apps/$app/main.c" "$src/userland/apps/$app/model.c"; then
        printf 'FAIL - %s still uses the windowd frontend API\n' "$app" >&2
        exit 1
    fi
done
grep -q 'libXm.so' "$work/sources"
for app in calc osver fileman taskmgr minesweeper leonmmcoset xiaobai; do
    desktop="$src/userland/apps/$app/$app.desktop"
    if [ ! -f "$desktop" ]; then
        printf 'FAIL - %s has no desktop entry\n' "$app" >&2
        exit 1
    fi
    grep -q '^Type=Application$' "$desktop"
    grep -q "^Exec=/usr/bin/$app\$" "$desktop"
done
printf 'ok - applications build only the Motif X11 frontend\n'
