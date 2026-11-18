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
# Legacy windowd clients must not return to the X11 product tree.
for app in apiapp browser bugtest cjktest devmgr downloadmgr drvmgr dynlinkerror \
    glxgears guitest hello helloworld httpget mp3play netctl notepad oshlp run \
    servicemgr settings sysconfdialog terminal uidemo; do
    if test -e "$src/userland/apps/$app"; then
        echo "FAIL - legacy windowd application source remains: $app" >&2
        exit 1
    fi
    if grep -qx "id = \"$app\"" "$src/configs/components.toml"; then
        echo "FAIL - removed application is still selectable: $app" >&2
        exit 1
    fi
    if grep -qx "$app" "$src/configs/nls/DOMAINS" "$src/configs/app-icons.list"; then
        echo "FAIL - removed application still has translation/icon metadata: $app" >&2
        exit 1
    fi
done
if test -e "$src/third_party/litehtml" || grep -q 'litehtml' "$src/.gitmodules" "$src/configs/dependencies.lock.json"; then
    echo 'FAIL - removed browser dependency remains registered' >&2
    exit 1
fi
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
printf 'ok - X11 product excludes legacy windowd applications and preserves X11 applications\n'
