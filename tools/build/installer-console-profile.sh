#!/bin/sh
# Remove the installed-system Xorg profile before it enters signed APK
# ownership. The live installer is assembled from the untouched raw root.
set -eu
[ "$#" = 1 ] || { echo 'usage: installer-console-profile.sh ROOT' >&2; exit 2; }
root=$1
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
    usr/lib/libportablegl.so.1; do
    remove_path "$path"
done

# These applications directly use X11/Motif/OpenGL and must not pull Xorg
# libraries back into reliefos-apps through the package's ELF dependencies.
for app in \
    calc doom doomlauncher fileman imageview installer leonmmcoset minesweeper osver \
    paint taskmgr xiaobai glxgears portablegl; do
    remove_path "usr/lib/reliefos/apps/$app"
    remove_path "usr/bin/$app"
    remove_path "usr/share/applications/reliefos-$app.desktop"
done

printf '%s\n' console >"$marker"
