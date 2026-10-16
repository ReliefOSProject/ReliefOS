#!/bin/sh
# Test installer-only payload at the signed-package boundary, not just its source.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd -P)
w=$(mktemp -d)
trap 'rm -rf "$w"' EXIT HUP INT TERM
${HOSTCC:-cc} -std=c11 -O2 -Wall -Wextra -Werror "$src/tools/host/manifest/reliefos-dedup.c" -o "$w/dedup"
export INSTALLER_DEDUP_TOOL="$w/dedup"
mkdir -p "$w/src/tools/build" "$w/src/docs" "$w/src/userland/apps/installer" "$w/src/system/xorg" \
    "$w/out/userland" "$w/out/userland-installer" \
    "$w/raw/usr/lib/reliefos/apps/settings" "$w/raw/usr/lib/leonos" "$w/raw/usr/bin" \
    "$w/raw/etc/reliefos/icewm" "$w/raw/etc/runlevels/default" \
    "$w/raw/etc/X11" "$w/raw/etc/pam.d" "$w/raw/usr/share/applications" \
    "$w/raw/usr/lib/reliefos/apps" \
    "$w/esp/EFI/BOOT" "$w/esp/reliefos" "$w/esp/leonos" "$w/esp/grub"
printf 'guide\n' > "$w/src/docs/ADVANCED_INSTALL.txt"
printf '[Desktop Entry]\n' > "$w/src/userland/apps/installer/installer.desktop"
printf '#!/bin/sh\n' > "$w/src/system/xorg/installer-session"
printf 'canonical runtime\n' > "$w/raw/usr/lib/reliefos/libreliefos.so.2"
printf 'compat runtime\n' > "$w/raw/usr/lib/leonos/libleonos.so.2"
printf 'settings\n' > "$w/raw/usr/lib/reliefos/apps/settings/settings.elf"
printf 'xorg\n' > "$w/raw/etc/reliefos/desktop-backend"
printf 'xorg server config\n' > "$w/raw/etc/X11/xorg.conf"
printf 'xdm conf\n' > "$w/raw/etc/reliefos/xdm.conf"
printf 'xdm servers\n' > "$w/raw/etc/reliefos/xdm-Xservers"
printf 'xdm resources\n' > "$w/raw/etc/reliefos/xdm-Xresources"
printf 'toolbar\n' > "$w/raw/etc/reliefos/icewm/toolbar"
printf 'icewm pam\n' > "$w/raw/etc/pam.d/xdm"
printf 'wallpaper\n' > "$w/raw/etc/reliefos/wallpaper.png"
for path in usr/lib/reliefos/reliefos-xdm usr/lib/reliefos/xorg-tty-wrapper \
    usr/lib/reliefos/xdm-session usr/lib/reliefos/xdm-setup; do
    mkdir -p "$w/raw/${path%/*}"
    printf 'Xorg helper\n' > "$w/raw/$path"
done
ln -s reliefos/icewm "$w/raw/etc/icewm"
for app in calc doom doomlauncher fileman imageview leonmmcoset minesweeper osver \
    paint taskmgr xiaobai glxgears portablegl; do
    mkdir -p "$w/raw/usr/lib/reliefos/apps/$app"
    printf '%s executable\n' "$app" > "$w/raw/usr/lib/reliefos/apps/$app/$app.elf"
    printf '%s manifest\n' "$app" > "$w/raw/usr/lib/reliefos/apps/$app/manifest.ini"
    ln -s "../lib/reliefos/apps/$app/$app.elf" "$w/raw/usr/bin/$app"
    printf '[Desktop Entry]\n' > "$w/raw/usr/share/applications/reliefos-$app.desktop"
done
printf 'portablegl library\n' > "$w/raw/usr/lib/libportablegl.so.1"
printf 'license\n' > "$w/raw/etc/license.conf"
ln -s ../../init.d/reliefos-audio "$w/raw/etc/runlevels/default/reliefos-audio"
printf 'gptinit\n' > "$w/out/userland-installer/gptinit.elf"
printf 'installer\n' > "$w/out/userland/installer.elf"
chmod 755 "$w/out/userland/installer.elf" "$w/out/userland-installer/gptinit.elf"
printf 'efi\n' > "$w/esp/EFI/BOOT/BOOTX64.EFI"
printf 'new-kernel\n' > "$w/esp/reliefos/kernel.sys"
printf 'legacy-kernel\n' > "$w/esp/leonos/kernel.sys"
printf 'menuentry reliefos\n' > "$w/esp/grub/grub.cfg"
cat > "$w/src/tools/build/apk-stage.sh" <<'ADAPTER'
#!/bin/sh
set -eu
[ "$#" = 12 ] || { echo "adapter: expected 12 arguments, got $#" >&2; exit 2; }
[ "$7" = fixture-deps ] || { echo "adapter: wrong dependency tool: $7" >&2; exit 2; }
[ "$8" = fixture-lock ] || { echo "adapter: wrong dependency lock: $8" >&2; exit 2; }
cp -a "$2" "$3"
mkdir -p "$3/lib/apk/db"
printf 'fixture package database\n' > "$3/lib/apk/db/installed"
ADAPTER
cp "$src/tools/build/installer-console-profile.sh" "$w/src/tools/build/installer-console-profile.sh"
APK_TOOL=fixture APK_UPSTREAM=fixture APK_DEPS_TOOL=fixture-deps APK_LOCK=fixture-lock \
 APK_OWN_TOOL=fixture APK_KEY=fixture APK_VERSION=fixture \
 sh "$src/tools/build/installer-stage.sh" "$w/src" "$w/out" "$w/raw" "$w/esp" "$w/stage" 1700000000
program=usr/lib/reliefos/apps/installer/installer.elf
[ -x "$w/stage/$program" ] || { echo 'FAIL - installer executable missing from runtime package'; exit 1; }
cmp "$w/out/userland/installer.elf" "$w/stage/$program"
[ "$(readlink "$w/stage/usr/bin/installer")" = ../lib/reliefos/apps/installer/installer.elf ]
[ -x "$w/stage/usr/lib/reliefos/apps/gptinit/gptinit.elf" ]
[ "$(readlink "$w/stage/usr/bin/gptinit")" = ../lib/reliefos/apps/gptinit/gptinit.elf ]
[ "$(cat "$w/stage/etc/reliefos/installer-runtime")" = installer ]
[ -x "$w/stage/usr/lib/reliefos/installer-session" ]
cmp "$w/src/system/xorg/installer-session" "$w/stage/usr/lib/reliefos/installer-session"
[ -f "$w/stage/usr/share/applications/reliefos-installer.desktop" ]
cmp "$w/src/userland/apps/installer/installer.desktop" \
    "$w/stage/usr/share/applications/reliefos-installer.desktop"
# The runtime root keeps the plain X11 policy of the raw root: the marker is
# never rewritten and the X session files are never stripped.
[ "$(cat "$w/stage/etc/reliefos/desktop-backend")" = xorg ]
[ "$(cat "$w/stage/install/root/etc/reliefos/desktop-backend")" = xorg ]
[ "$(cat "$w/stage/install/root-console/etc/reliefos/desktop-backend")" = console ]
[ -f "$w/stage/etc/reliefos/xdm.conf" ]
[ -f "$w/stage/etc/reliefos/icewm/toolbar" ]
[ -f "$w/stage/install/root/etc/reliefos/xdm.conf" ]
[ -f "$w/stage/install/root/usr/lib/reliefos/apps/calc/calc.elf" ]
[ ! -e "$w/stage/install/root-console/etc/X11/xorg.conf" ]
[ ! -e "$w/stage/install/root-console/etc/reliefos/xdm.conf" ]
[ ! -e "$w/stage/install/root-console/etc/reliefos/xdm-Xservers" ]
[ ! -e "$w/stage/install/root-console/etc/reliefos/xdm-Xresources" ]
[ ! -e "$w/stage/install/root-console/etc/reliefos/icewm" ]
[ ! -e "$w/stage/install/root-console/etc/icewm" ]
[ ! -e "$w/stage/install/root-console/etc/pam.d/xdm" ]
[ ! -e "$w/stage/install/root-console/etc/reliefos/wallpaper.png" ]
for path in usr/lib/reliefos/reliefos-xdm usr/lib/reliefos/xorg-tty-wrapper \
    usr/lib/reliefos/xdm-session usr/lib/reliefos/xdm-setup; do
    [ ! -e "$w/stage/install/root-console/$path" ]
done
for app in calc doom doomlauncher fileman imageview leonmmcoset minesweeper osver \
    paint taskmgr xiaobai glxgears portablegl; do
    [ ! -e "$w/stage/install/root-console/usr/lib/reliefos/apps/$app" ]
    [ ! -e "$w/stage/install/root-console/usr/bin/$app" ]
    [ ! -e "$w/stage/install/root-console/usr/share/applications/reliefos-$app.desktop" ]
done
[ ! -e "$w/stage/install/root-console/usr/lib/libportablegl.so.1" ]
[ -d "$w/stage/install/root-console/usr/lib/reliefos/apps/settings" ]
[ -f "$w/stage/install/root-console/usr/lib/reliefos/libreliefos.so.2" ]
[ -f "$w/stage/etc/license.conf" ]
inode() { stat -c '%d:%i' "$1"; }
library=$w/stage/usr/lib/reliefos/libreliefos.so.2
installed_library=$w/stage/install/root/usr/lib/reliefos/libreliefos.so.2
console_library=$w/stage/install/root-console/usr/lib/reliefos/libreliefos.so.2
[ "$(inode "$library")" = "$(inode "$installed_library")" ]
[ "$(inode "$library")" = "$(inode "$console_library")" ]
runtime_db=$w/stage/lib/apk/db/installed
installed_db=$w/stage/install/root/lib/apk/db/installed
console_db=$w/stage/install/root-console/lib/apk/db/installed
[ "$(inode "$runtime_db")" != "$(inode "$installed_db")" ]
[ "$(inode "$runtime_db")" != "$(inode "$console_db")" ]
[ "$(inode "$installed_db")" != "$(inode "$console_db")" ]
runtime_marker=$w/stage/etc/reliefos/desktop-backend
installed_marker=$w/stage/install/root/etc/reliefos/desktop-backend
console_marker=$w/stage/install/root-console/etc/reliefos/desktop-backend
[ "$(inode "$runtime_marker")" != "$(inode "$installed_marker")" ]
[ "$(inode "$runtime_marker")" != "$(inode "$console_marker")" ]
[ "$(inode "$installed_marker")" != "$(inode "$console_marker")" ]
# No windowd stack is staged or revived on either root.
[ ! -e "$w/stage/etc/runlevels/default/reliefos-windowd" ]
[ ! -e "$w/stage/etc/runlevels/default/reliefos-session" ]
[ ! -e "$w/stage/etc/runlevels/default/reliefos-imd" ]
[ ! -e "$w/stage/usr/lib/reliefos/apps/windowd" ]
[ ! -e "$w/stage/usr/lib/reliefos/apps/desktop" ]
[ ! -e "$w/stage/install/root/usr/lib/reliefos/apps/desktop" ]
# Runtime libraries and installed apps are the raw root's own; nothing is
# overridden with an installer-policy build.
cmp "$w/raw/usr/lib/reliefos/libreliefos.so.2" "$w/stage/usr/lib/reliefos/libreliefos.so.2"
cmp "$w/raw/usr/lib/reliefos/libreliefos.so.2" "$w/stage/install/root/usr/lib/reliefos/libreliefos.so.2"
cmp "$w/raw/usr/lib/leonos/libleonos.so.2" "$w/stage/install/root/usr/lib/leonos/libleonos.so.2"
cmp "$w/raw/usr/lib/reliefos/apps/settings/settings.elf" \
    "$w/stage/install/root/usr/lib/reliefos/apps/settings/settings.elf"
[ ! -e "$w/stage/install/root/$program" ]
[ ! -e "$w/stage/install/root/etc/license.conf" ]
[ -f "$w/stage/install/esp/reliefos/kernel.sys" ]
[ -f "$w/stage/install/esp/leonos/kernel.sys" ]
[ -f "$w/stage/install/esp/grub/grub.cfg" ]
echo 'installer stage: executable, X11 live policy and canonical/rollback boot kernels are packaged'
