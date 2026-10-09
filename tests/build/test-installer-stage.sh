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
    "$w/esp/EFI/BOOT" "$w/esp/reliefos" "$w/esp/leonos" "$w/esp/grub"
printf 'guide\n' > "$w/src/docs/ADVANCED_INSTALL.txt"
printf '[Desktop Entry]\n' > "$w/src/userland/apps/installer/installer.desktop"
printf '#!/bin/sh\n' > "$w/src/system/xorg/installer-session"
printf 'canonical runtime\n' > "$w/raw/usr/lib/reliefos/libreliefos.so.2"
printf 'compat runtime\n' > "$w/raw/usr/lib/leonos/libleonos.so.2"
printf 'settings\n' > "$w/raw/usr/lib/reliefos/apps/settings/settings.elf"
printf 'xorg\n' > "$w/raw/etc/reliefos/desktop-backend"
printf 'xdm conf\n' > "$w/raw/etc/reliefos/xdm.conf"
printf 'toolbar\n' > "$w/raw/etc/reliefos/icewm/toolbar"
printf 'license\n' > "$w/raw/etc/license.conf"
ln -s ../../init.d/reliefos-audio "$w/raw/etc/runlevels/default/reliefos-audio"
printf 'gptinit\n' > "$w/out/userland-installer/gptinit.elf"
printf 'installer\n' > "$w/out/userland/installer.elf"
chmod 755 "$w/out/userland/installer.elf" "$w/out/userland-installer/gptinit.elf"
printf 'efi\n' > "$w/esp/EFI/BOOT/BOOTX64.EFI"
printf 'new-loader\n' > "$w/esp/reliefos/loader.elf"
printf 'new-kernel\n' > "$w/esp/reliefos/kernel.sys"
printf 'legacy-loader\n' > "$w/esp/loader.elf"
printf 'legacy-kernel\n' > "$w/esp/leonos/kernel.sys"
printf 'menuentry reliefos\n' > "$w/esp/grub/grub.cfg"
cat > "$w/src/tools/build/apk-stage.sh" <<'ADAPTER'
#!/bin/sh
set -eu
[ "$#" = 12 ] || { echo "adapter: expected 12 arguments, got $#" >&2; exit 2; }
[ "$7" = fixture-deps ] || { echo "adapter: wrong dependency tool: $7" >&2; exit 2; }
[ "$8" = fixture-lock ] || { echo "adapter: wrong dependency lock: $8" >&2; exit 2; }
cp -a "$2" "$3"
ADAPTER
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
[ -f "$w/stage/etc/reliefos/xdm.conf" ]
[ -f "$w/stage/etc/reliefos/icewm/toolbar" ]
[ -f "$w/stage/etc/license.conf" ]
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
[ -f "$w/stage/install/esp/reliefos/loader.elf" ]
[ -f "$w/stage/install/esp/reliefos/kernel.sys" ]
[ -f "$w/stage/install/esp/leonos/kernel.sys" ]
[ -f "$w/stage/install/esp/loader.elf" ]
[ -f "$w/stage/install/esp/grub/grub.cfg" ]
echo 'installer stage: executable, X11 live policy and paired boot layouts are packaged'
