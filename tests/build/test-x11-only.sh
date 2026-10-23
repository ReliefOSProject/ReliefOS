#!/bin/sh
# The supported graphical product must not revive a retired window stack.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
for path in userland/apps/desktop userland/apps/windowd \
    userland/runtime/src/wind.c \
    userland/runtime/include/reliefos/windowd.h \
    userland/runtime/include/leonos/windowd.h \
    userland/runtime/include/reliefos/gui.h \
    userland/runtime/include/leonos/gui.h \
    userland/runtime/include/reliefos/mouse.h \
    userland/runtime/include/leonos/mouse.h \
    system/config/taskbar.cfg; do
    if test -e "$src/$path"; then
        echo "FAIL - removed implementation still exists: $path" >&2
        exit 1
    fi
done
# Applications are source archives until a supported frontend is available.
for app in settings terminal run netctl servicemgr devmgr drvmgr sysconfdialog \
    apiapp hello uidemo cjktest notepad bugtest httpget downloadmgr browser \
    mp3play oshlp helloworld guitest glxgears dynlinkerror; do
    test -f "$src/userland/apps/$app/main.c"
    if grep -qx "id = \"$app\"" "$src/configs/components.toml"; then
        echo "FAIL - unavailable application is still selectable: $app" >&2
        exit 1
    fi
done
if grep -Eq 'APP_(DESKTOP|WINDOWD)_' "$src/Kconfig.components"; then
    echo 'FAIL - removed configuration is still present' >&2
    exit 1
fi
if grep -Eq 'wind[.]o|ui[.]o|ui_surface[.]o' "$src/userland/runtime/src/Kbuild"; then
    echo 'FAIL - removed runtime is still linked' >&2
    exit 1
fi
for app in calc fileman taskmgr reliefapk reliefmail reliefword reliefexcel \
    reliefppt reliefmath reliefmathpp relieftodo reliefdiagram reliefcapture reliefplayer; do
    grep -qx "id = \"$app\"" "$src/configs/components.toml"
done
printf 'ok - X11 product excludes the removed stack and preserves application sources\n'
