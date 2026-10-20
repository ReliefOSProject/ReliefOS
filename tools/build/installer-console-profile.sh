#!/bin/sh
# Remove the installed-system Xorg profile before it enters signed APK
# ownership. The live installer is assembled from the untouched raw root.
set -eu
[ "$#" = 1 ] || { echo 'usage: installer-console-profile.sh ROOT' >&2; exit 2; }
root=$1
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd -P)
fltk_apps=$src/configs/fltk-apps.list
[ -f "$fltk_apps" ] && [ ! -L "$fltk_apps" ] || {
    echo "console profile requires the FLTK application list: $fltk_apps" >&2
    exit 1
}
[ -d "$root" ] && [ ! -L "$root" ] || {
    echo "console profile root is missing or not a directory: $root" >&2
    exit 1
}

marker=$root/etc/reliefos/desktop-backend
[ -f "$marker" ] && [ ! -L "$marker" ] || {
    echo "console profile requires a regular desktop backend marker: $marker" >&2
    exit 1
}
marker_value=
marker_lines=0
while IFS= read -r marker_line || [ -n "$marker_line" ]; do
    marker_lines=$((marker_lines + 1))
    [ "$marker_lines" -gt 1 ] || marker_value=$marker_line
    marker_line=
done <"$marker"
[ "$marker_lines" = 1 ] && [ "$marker_value" = xorg ] || {
    echo "console profile expects the xorg raw root marker: $marker" >&2
    exit 1
}

remove_path() {
    relative=$1
    case $relative in
        /*|../*|*/../*|*/..|*//*)
            echo "unsafe console profile path: $relative" >&2
            exit 1
            ;;
    esac
    target=$root/$relative
    parent=${target%/*}
    while [ "$parent" != "$root" ]; do
        [ ! -L "$parent" ] || {
            echo "console profile path has a symlink parent: $relative" >&2
            exit 1
        }
        [ -d "$parent" ] || return 0
        parent=${parent%/*}
    done
    rm -rf "$target"
}

for path in \
    etc/X11/xorg.conf \
    etc/X11/xdm \
    usr/lib/X11/xdm \
    etc/reliefos/xdm.conf \
    etc/reliefos/xdm-Xservers \
    etc/reliefos/xdm-Xresources \
    etc/reliefos/xdm-session \
    etc/reliefos/icewm \
    etc/reliefos/wallpaper.png \
    etc/pam.d/xdm \
    etc/icewm \
    usr/lib/reliefos/reliefos-xdm \
    usr/lib/reliefos/xorg-tty-wrapper \
    usr/lib/reliefos/xdm-session \
    usr/lib/reliefos/xdm-setup \
    usr/lib/libportablegl.so.1 \
    usr/share/reliefmath \
    usr/share/licenses/reliefmath \
    usr/share/mime/packages/reliefmath.xml \
    usr/share/mime/packages/reliefmathpp.xml \
    usr/share/mime/packages/relieftodo.xml \
    usr/share/mime/packages/reliefdiagram.xml; do
    remove_path "$path"
done

remove_app() {
    app=$1
    remove_path "usr/lib/reliefos/apps/$app"
    remove_path "usr/bin/$app"
    remove_path "usr/share/applications/reliefos-$app.desktop"
    for size in 16 32 48 64 128 256; do
        remove_path "usr/share/icons/hicolor/${size}x${size}/apps/reliefos-$app.png"
    done
}

# Use the build's FLTK application list so new graphical frontends cannot
# retain Xorg ELF dependencies in the console-only reliefos-apps package.
while IFS= read -r app || [ -n "$app" ]; do
    case $app in
        ''|*[!a-z0-9_-]*)
            echo "invalid FLTK application name in $fltk_apps: $app" >&2
            exit 1
            ;;
    esac
    remove_app "$app"
done <"$fltk_apps"
for app in glxgears portablegl; do
    remove_app "$app"
done

printf '%s\n' console >"$marker"
