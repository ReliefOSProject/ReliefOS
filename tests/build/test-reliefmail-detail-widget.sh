#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
target="$src/out/xorg-vmware-debug"
sdk="$target/sdk/reliefos-musl-sdk"
fltk="$target/upstream/x11-development/usr"
binary="$src/build/reliefmail-dev/task8a-fixround2/detail-widget/reliefmail-detail-widget"
mkdir -p "$(dirname "$binary")"
fontconfig="$src/build/reliefmail-dev/task8a-fixround2/detail-widget/fontconfig.xml"
fontconfig_path="$src/build/reliefmail-dev/task8a-fixround2/detail-widget/fontconfig"
widget_source="${RELIEFMAIL_WIDGET_SOURCE:-$src/userland/apps/reliefmail/ui_widgets.cpp}"
mkdir -p "$fontconfig_path"
cat >"$fontconfig" <<'EOF'
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<fontconfig>
  <dir>FONT_DIR_PLACEHOLDER</dir>
  <cachedir>/tmp</cachedir>
</fontconfig>
EOF
sed "s|FONT_DIR_PLACEHOLDER|$target/stage/disk-root/usr/share/fonts|" \
    "$fontconfig" >"$fontconfig_path/fonts.conf"

"$sdk/bin/reliefos-musl-cc" -x c++ -std=c++17 -nostdinc++ \
    -fno-exceptions -fno-rtti -O1 -Wall -Wextra -Werror \
    -Wno-unused-command-line-argument -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE \
    -I"$src/userland/apps/reliefmail" -I"$src/userland/apps" \
    -I"$src/userland/runtime/include" -I"$src/include" -I"$target/include" \
    -I"$sdk/include" -isystem "$fltk/include" \
    -include "$target/include/generated/autoconf.h" \
    "$src/tests/host/test_reliefmail_detail_widget.cpp" \
    "$widget_source" \
    "$src/userland/apps/reliefmail/model.cpp" \
    -x none \
    "$fltk/lib/libfltk.so" "$fltk/lib/libstdc++.so.6" "$fltk/lib/libgcc_s.so.1" \
    -L"$target/stage/disk-root/usr/lib" -l:libreliefos.so.2 -l:libX11.so.6 -ldl -lm \
    -Wl,-rpath,/usr/lib/reliefos:/lib:/usr/lib -o "$binary"

runtime_path="$target/stage/disk-root/lib:$target/stage/disk-root/usr/lib:$target/stage/disk-root/usr/lib/reliefos:$fltk/lib:$target/sysroot/musl/lib:$sdk/lib"
xvfb-run -a env LD_LIBRARY_PATH="$runtime_path" \
    FONTCONFIG_FILE="$fontconfig_path/fonts.conf" FONTCONFIG_PATH="$fontconfig_path" \
    "$target/sysroot/musl/lib/ld-musl-x86_64.so.1" \
    --library-path "$runtime_path" "$binary"
