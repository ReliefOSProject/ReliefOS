#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
tmp_root=${TMPDIR:-$src/out/tmp}
mkdir -p "$tmp_root"
work=$(mktemp -d "$tmp_root/reliefos-fltk-apps.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
if [ ! -f "$src/userland/apps/calc/engine.c" ] || [ ! -f "$src/userland/apps/osver/debug_click.h" ]; then
    printf 'FAIL - calculator and system information have no toolkit-independent behavior boundary\n' >&2
    exit 1
fi
for app in taskmgr minesweeper leonmmcoset xiaobai paint imageview doomlauncher installer; do
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
    "$src/userland/apps/xiaobai/model.c" "$src/userland/apps/paint/model.c" \
    "$src/userland/apps/imageview/model.c" "$src/userland/apps/doomlauncher/model.c" \
    "$src/userland/apps/installer/model.c" \
    -o "$work/test"
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
	@printf '%s\n' '$(call userland_sources,calc)' '$(call userland_sources,osver)' '$(call userland_sources,fileman)' '$(call userland_sources,taskmgr)' '$(call userland_sources,minesweeper)' '$(call userland_sources,leonmmcoset)' '$(call userland_sources,xiaobai)' '$(call userland_sources,paint)' '$(call userland_sources,imageview)' '$(call userland_sources,doom)' '$(call userland_sources,doomlauncher)' '$(call userland_sources,installer)' '$(call userland_sources,reliefexcel)'
	@printf '%s\n' '$(USERLAND_LIBS_calc)' '$(USERLAND_LIBS_osver)' '$(USERLAND_LIBS_fileman)' '$(USERLAND_LIBS_taskmgr)' '$(USERLAND_LIBS_minesweeper)' '$(USERLAND_LIBS_leonmmcoset)' '$(USERLAND_LIBS_xiaobai)' '$(USERLAND_LIBS_paint)' '$(USERLAND_LIBS_imageview)' '$(USERLAND_LIBS_doomlauncher)' '$(USERLAND_LIBS_doom)' '$(USERLAND_LIBS_installer)' '$(USERLAND_LIBS_reliefexcel)'
	@printf '%s\n' '$(USERLAND_CXX_FLAGS)' '$(USERLAND_CXX_EXTRA_calc)'
MAKE
make -s -f "$work/sources.mk" ROOT="$src" WORK="$work" sources >"$work/sources"
for app in calc osver fileman taskmgr minesweeper leonmmcoset xiaobai paint imageview doomlauncher doom installer reliefexcel; do
    grep -q "userland/apps/$app/main.cpp" "$work/sources"
    if grep -Eq "userland/apps/$app/main\\.c([[:space:]]|$)" "$work/sources"; then
        printf 'FAIL - %s still selects a C frontend instead of the FLTK C++ frontend\n' "$app" >&2
        exit 1
    fi
    for removed in native.c input.c view.c; do
        if grep -q "userland/apps/$app/$removed" "$work/sources"; then
            printf 'FAIL - %s still selects native frontend source %s\n' "$app" "$removed" >&2
            exit 1
        fi
    done
    if ! grep -q 'fltk_support.h\|fltk_png_app.h' "$src/userland/apps/$app/main.cpp" ||
       { ! grep -q 'reliefos_fltk::init' "$src/userland/apps/$app/main.cpp" &&
         ! grep -q 'init();' "$src/userland/apps/fltk_png_app.h"; }; then
        printf 'FAIL - %s does not initialize the shared FLTK locale/font support\n' "$app" >&2
        exit 1
    fi
done
grep -q 'userland/apps/reliefexcel/main.cpp' "$work/sources"
for old_file in motif.c motif_dialogs.c motif.h; do
    if [ -e "$src/userland/apps/fileman/$old_file" ]; then
        printf 'FAIL - fileman still carries the obsolete Motif frontend file %s\n' "$old_file" >&2
        exit 1
    fi
done
grep -q '^Categories=Office;Spreadsheet;$' "$src/userland/apps/reliefexcel/reliefexcel.desktop"
grep -q '^Exec=/usr/bin/reliefexcel %f$' "$src/userland/apps/reliefexcel/reliefexcel.desktop"
grep -q '^Icon=/usr/lib/reliefos/apps/reliefexcel/reliefexcel.bmp$' "$src/userland/apps/reliefexcel/reliefexcel.desktop"
[ -s "$src/resources/build-art/app-icons/reliefexcel.bmp" ]
for app in taskmgr minesweeper leonmmcoset xiaobai paint imageview doomlauncher installer; do
    grep -q "userland/apps/$app/model.c" "$work/sources"
    if grep -q 'reliefos_gui_create_app_window\|reliefos_ui_' \
        "$src/userland/apps/$app/model.c"; then
        printf 'FAIL - %s business model still uses the windowd frontend API\n' "$app" >&2
        exit 1
    fi
done
grep -q 'Fl::set_font.*SimSun' "$src/userland/apps/fltk_support.h"
grep -q 'gettext(text)' "$src/userland/apps/fltk_support.h"
grep -q 'libfltk.so' "$work/sources"
grep -q 'libstdc++.so.6' "$work/sources"
grep -q 'libgcc_s.so.1' "$work/sources"
grep -q -- '-std=c++17' "$work/sources"
grep -q -- '-nostdinc++' "$work/sources"
grep -q -- '-fno-exceptions' "$work/sources"
grep -q -- '-fno-rtti' "$work/sources"
grep -Fq -- "-I$src/userland/apps -isystem $work/out/upstream/x11-development/usr/include" "$work/sources"
if grep -q 'libXm.so\|libXt.so' "$work/sources"; then
    printf 'FAIL - FLTK apps still link Motif or Xt\n' >&2
    exit 1
fi
for app in calc osver fileman taskmgr minesweeper leonmmcoset xiaobai paint imageview doom doomlauncher installer; do
    desktop="$src/userland/apps/$app/$app.desktop"
    if [ ! -f "$desktop" ]; then
        printf 'FAIL - %s has no desktop entry\n' "$app" >&2
        exit 1
    fi
    grep -q '^Type=Application$' "$desktop"
    if ! grep -qE "^Exec=/usr/bin/$app( --graphical)?\$" "$desktop"; then
        printf 'FAIL - %s desktop entry does not launch the app\n' "$app" >&2
        exit 1
    fi
done
printf 'ok - applications build the FLTK frontend and retain C behavior models\n'
