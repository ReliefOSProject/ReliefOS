#!/bin/sh
# Test installer-only payload at the signed-package boundary, not just its source.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd -P)
w=$(mktemp -d)
trap 'rm -rf "$w"' EXIT HUP INT TERM
${HOSTCC:-cc} -std=c11 -O2 -Wall -Wextra -Werror "$src/tools/host/manifest/reliefos-dedup.c" -o "$w/dedup"
export INSTALLER_DEDUP_TOOL="$w/dedup"
mkdir -p "$w/src/tools/build" "$w/src/Documentation/admin-guide" "$w/src/configs" "$w/src/userland/apps/installer" "$w/src/system/xorg" \
    "$w/out/userland" "$w/out/userland-installer" \
    "$w/raw/usr/lib/reliefos/apps/soundctl" "$w/raw/usr/lib/leonos" "$w/raw/usr/bin" \
    "$w/raw/etc/reliefos/icewm" "$w/raw/etc/runlevels/default" \
    "$w/raw/etc/X11/xdm" "$w/raw/etc/pam.d" "$w/raw/usr/share/applications" \
    "$w/raw/usr/lib/X11/xdm" \
    "$w/raw/usr/lib/reliefos/apps" "$w/raw/usr/share/reliefmath" \
    "$w/raw/usr/share/licenses/reliefmath" "$w/raw/usr/share/mime/packages" \
    "$w/raw/usr/share/fonts/reliefos" "$w/raw/usr/share/reliefos" \
    "$w/esp/EFI/BOOT" "$w/esp/reliefos" "$w/esp/leonos" "$w/esp/grub"
printf 'guide\n' > "$w/src/Documentation/admin-guide/installation.rst"
cp "$src/configs/fltk-apps.list" "$w/src/configs/fltk-apps.list"
printf 'future-fltk\n' >> "$w/src/configs/fltk-apps.list"
printf '[Desktop Entry]\n' > "$w/src/userland/apps/installer/installer.desktop"
cp "$src/system/xorg/installer-session" "$w/src/system/xorg/installer-session"
ln -s "$src/resources" "$w/src/resources"
printf 'canonical runtime\n' > "$w/raw/usr/lib/reliefos/libreliefos.so.2"
printf 'compat runtime\n' > "$w/raw/usr/lib/leonos/libleonos.so.2"
printf 'sound controls\n' > "$w/raw/usr/lib/reliefos/apps/soundctl/soundctl.elf"
printf 'xorg\n' > "$w/raw/etc/reliefos/desktop-backend"
printf 'xorg server config\n' > "$w/raw/etc/X11/xorg.conf"
printf 'xdm conf\n' > "$w/raw/etc/X11/xdm/xdm-config"
printf 'xdm servers\n' > "$w/raw/etc/X11/xdm/Xservers"
printf 'xdm resources\n' > "$w/raw/etc/X11/xdm/Xresources"
printf '#!/bin/sh\nexit 0\n' > "$w/raw/usr/lib/X11/xdm/Xsession"
printf '#!/bin/sh\nexit 0\n' > "$w/raw/usr/lib/X11/xdm/Xsetup_0"
chmod 755 "$w/raw/usr/lib/X11/xdm/Xsession" "$w/raw/usr/lib/X11/xdm/Xsetup_0"
printf 'toolbar\n' > "$w/raw/etc/reliefos/icewm/toolbar"
printf 'icewm pam\n' > "$w/raw/etc/pam.d/xdm"
printf 'wallpaper\n' > "$w/raw/etc/reliefos/wallpaper.png"
for path in usr/lib/reliefos/reliefos-xdm usr/lib/reliefos/xorg-tty-wrapper; do
    mkdir -p "$w/raw/${path%/*}"
    printf 'Xorg helper\n' > "$w/raw/$path"
done
ln -s reliefos/icewm "$w/raw/etc/icewm"
for app in $(cat "$w/src/configs/fltk-apps.list"); do
    [ "$app" != installer ] || continue
    mkdir -p "$w/raw/usr/lib/reliefos/apps/$app"
    printf '%s executable\n' "$app" > "$w/raw/usr/lib/reliefos/apps/$app/$app.elf"
    printf '%s manifest\n' "$app" > "$w/raw/usr/lib/reliefos/apps/$app/manifest.ini"
    ln -s "../lib/reliefos/apps/$app/$app.elf" "$w/raw/usr/bin/$app"
    printf '[Desktop Entry]\n' > "$w/raw/usr/share/applications/reliefos-$app.desktop"
    for size in 16 32 48 64 128 256; do
        icons=usr/share/icons/hicolor/${size}x${size}/apps
        mkdir -p "$w/raw/$icons"
        printf '%s icon\n' "$app" > "$w/raw/$icons/reliefos-$app.png"
    done
done
printf 'math font\n' > "$w/raw/usr/share/reliefmath/STIXTwoMath-Regular.otf"
printf 'font license\n' > "$w/raw/usr/share/licenses/reliefmath/STIX-OFL.txt"
printf 'license summary\n' > "$w/raw/usr/share/licenses/reliefmath/README.md"
printf '<mime-info/>\n' > "$w/raw/usr/share/mime/packages/reliefmath.xml"
printf 'shared CJK font\n' > "$w/raw/usr/share/fonts/reliefos/simsun.ttc"
printf 'unrelated app data\n' > "$w/raw/usr/share/reliefos/other-app-data"
printf 'worker executable\n' > "$w/raw/usr/lib/reliefos/apps/reliefmath/reliefmath-render.elf"
printf 'window icon\n' > "$w/raw/usr/lib/reliefos/apps/reliefmath/reliefmath.bmp"
for size in 16 32 48 64 128 256; do
    mkdir -p "$w/raw/usr/share/icons/hicolor/${size}x${size}/apps"
    printf 'math icon\n' > "$w/raw/usr/share/icons/hicolor/${size}x${size}/apps/reliefos-reliefmath.png"
done
printf 'license\n' > "$w/raw/etc/license.conf"
ln -s ../../init.d/reliefos-audio "$w/raw/etc/runlevels/default/reliefos-audio"
printf 'gptinit\n' > "$w/out/userland-installer/gptinit.elf"
printf 'installer\n' > "$w/out/userland/installer.elf"
for path in usr/sbin/reliefos-boot-install usr/sbin/grub-install \
    usr/sbin/grub-bios-setup usr/sbin/grub-probe usr/bin/grub-mkimage \
    usr/lib/grub/i386-pc/boot.img; do
    mkdir -p "$w/raw/${path%/*}"
    printf 'boot tool fixture\n' > "$w/raw/$path"
done
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
grep -q '/usr/bin/icewm-session' "$w/stage/usr/lib/reliefos/installer-session"
if grep -q 'xwallpaper' "$w/stage/usr/lib/reliefos/installer-session"; then
    echo 'live installer session must leave its background to the IceWM theme' >&2
    exit 1
fi
[ -f "$w/stage/usr/share/applications/reliefos-installer.desktop" ]
cmp "$w/src/userland/apps/installer/installer.desktop" \
    "$w/stage/usr/share/applications/reliefos-installer.desktop"
cmp "$src/resources/build-art/app-icons/installer.bmp" "$w/stage/usr/lib/reliefos/apps/installer/installer.bmp"
for size in 16 32 48 64 128 256; do
    icon=usr/share/icons/hicolor/${size}x${size}/apps/reliefos-installer.png
    cmp "$src/resources/build-art/app-icons/hicolor/${size}x${size}/apps/reliefos-installer.png" "$w/stage/$icon"
    [ ! -e "$w/stage/install/root-console/$icon" ]
done
# The runtime root keeps the plain X11 policy of the raw root: the marker is
# never rewritten and the X session files are never stripped.
[ "$(cat "$w/stage/etc/reliefos/desktop-backend")" = xorg ]
[ "$(cat "$w/stage/install/root/etc/reliefos/desktop-backend")" = xorg ]
[ "$(cat "$w/stage/install/root-console/etc/reliefos/desktop-backend")" = console ]
for profile in "$w/stage" "$w/stage/install/root" "$w/stage/install/root-console"; do
    for path in usr/sbin/reliefos-boot-install usr/sbin/grub-install \
        usr/sbin/grub-bios-setup usr/sbin/grub-probe usr/bin/grub-mkimage \
        usr/lib/grub/i386-pc/boot.img; do
        cmp "$w/raw/$path" "$profile/$path"
    done
done
for path in etc/X11/xdm/xdm-config etc/X11/xdm/Xservers \
    etc/X11/xdm/Xresources usr/lib/X11/xdm/Xsession \
    usr/lib/X11/xdm/Xsetup_0; do
    [ -f "$w/stage/$path" ]
    [ -f "$w/stage/install/root/$path" ]
    [ ! -e "$w/stage/install/root-console/$path" ]
done
# No package root installs the former ReliefOS-specific XDM config or script
# paths after the move to XDM's standard directories.
for root in "$w/stage" "$w/stage/install/root" "$w/stage/install/root-console"; do
    for path in etc/reliefos/xdm.conf etc/reliefos/xdm-Xservers \
        etc/reliefos/xdm-Xresources etc/reliefos/xdm-session \
        etc/reliefos/xdm-setup usr/lib/reliefos/xdm-session \
        usr/lib/reliefos/xdm-setup; do
        [ ! -e "$root/$path" ]
    done
done
[ -f "$w/stage/etc/reliefos/icewm/toolbar" ]
[ -f "$w/stage/install/root/usr/lib/reliefos/apps/calc/calc.elf" ]
[ ! -e "$w/stage/install/root-console/etc/X11/xorg.conf" ]
[ ! -e "$w/stage/install/root-console/etc/X11/xdm/xdm-config" ]
[ ! -e "$w/stage/install/root-console/etc/X11/xdm/Xservers" ]
[ ! -e "$w/stage/install/root-console/etc/X11/xdm/Xresources" ]
[ ! -e "$w/stage/install/root-console/usr/lib/X11/xdm/Xsession" ]
[ ! -e "$w/stage/install/root-console/usr/lib/X11/xdm/Xsetup_0" ]
[ ! -e "$w/stage/install/root-console/etc/reliefos/icewm" ]
[ ! -e "$w/stage/install/root-console/etc/icewm" ]
[ ! -e "$w/stage/install/root-console/etc/pam.d/xdm" ]
[ ! -e "$w/stage/install/root-console/etc/reliefos/wallpaper.png" ]
for path in usr/lib/reliefos/reliefos-xdm usr/lib/reliefos/xorg-tty-wrapper; do
    [ ! -e "$w/stage/install/root-console/$path" ]
done
for app in $(cat "$w/src/configs/fltk-apps.list"); do
    [ ! -e "$w/stage/install/root-console/usr/lib/reliefos/apps/$app" ]
    [ ! -e "$w/stage/install/root-console/usr/bin/$app" ]
    [ ! -e "$w/stage/install/root-console/usr/share/applications/reliefos-$app.desktop" ]
    [ "$app" != installer ] || continue
    for profile in "$w/stage" "$w/stage/install/root"; do
        cmp "$w/raw/usr/lib/reliefos/apps/$app/$app.elf" "$profile/usr/lib/reliefos/apps/$app/$app.elf"
        [ -L "$profile/usr/bin/$app" ]
        [ -f "$profile/usr/share/applications/reliefos-$app.desktop" ]
        for size in 16 32 48 64 128 256; do
            icon=usr/share/icons/hicolor/${size}x${size}/apps/reliefos-$app.png
            cmp "$w/raw/$icon" "$profile/$icon"
            [ ! -e "$w/stage/install/root-console/$icon" ]
        done
    done
done
for path in \
    usr/lib/reliefos/apps/reliefmath \
    usr/bin/reliefmath \
    usr/share/applications/reliefos-reliefmath.desktop \
    usr/share/reliefmath \
    usr/share/licenses/reliefmath \
    usr/share/mime/packages/reliefmath.xml; do
    [ ! -e "$w/stage/install/root-console/$path" ] || {
        echo "ReliefMath asset entered console root: $path" >&2
        exit 1
    }
done
for size in 16 32 48 64 128 256; do
    [ ! -e "$w/stage/install/root-console/usr/share/icons/hicolor/${size}x${size}/apps/reliefos-reliefmath.png" ]
done
[ -f "$w/stage/install/root/usr/lib/reliefos/apps/reliefmath/reliefmath-render.elf" ]
[ -f "$w/stage/install/root/usr/share/reliefmath/STIXTwoMath-Regular.otf" ]
[ -f "$w/stage/install/root/usr/share/licenses/reliefmath/STIX-OFL.txt" ]
[ -f "$w/stage/install/root/usr/share/mime/packages/reliefmath.xml" ]
[ -f "$w/stage/install/root-console/usr/share/fonts/reliefos/simsun.ttc" ]
[ -f "$w/stage/install/root-console/usr/share/reliefos/other-app-data" ]
[ -d "$w/stage/install/root-console/usr/lib/reliefos/apps/soundctl" ]
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
cmp "$w/raw/usr/lib/reliefos/apps/soundctl/soundctl.elf" \
    "$w/stage/install/root/usr/lib/reliefos/apps/soundctl/soundctl.elf"
[ ! -e "$w/stage/install/root/$program" ]
[ ! -e "$w/stage/install/root/etc/license.conf" ]
[ -f "$w/stage/install/esp/reliefos/kernel.sys" ]
[ -f "$w/stage/install/esp/leonos/kernel.sys" ]
[ -f "$w/stage/install/esp/grub/grub.cfg" ]
echo 'installer stage: executable, X11 live policy and canonical/rollback boot kernels are packaged'
