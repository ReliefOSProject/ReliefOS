#!/bin/sh
# Test installer-only payload at the signed-package boundary, not just its source.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd -P)
w=$(mktemp -d)
trap 'rm -rf "$w"' EXIT HUP INT TERM
${HOSTCC:-cc} -std=c11 -O2 -Wall -Wextra -Werror "$src/tools/host/manifest/reliefos-dedup.c" -o "$w/dedup"
export INSTALLER_DEDUP_TOOL="$w/dedup"
mkdir -p "$w/src/tools/build" "$w/src/docs" "$w/out/userland" "$w/out/userland-installer" "$w/out/userland-installer-policy" "$w/out/installer/lib" "$w/raw/usr/lib/reliefos/apps/desktop" "$w/raw/usr/lib/reliefos/apps/settings" "$w/raw/usr/lib/leonos" "$w/raw/usr/lib/reliefos" "$w/raw/usr/bin" "$w/esp/EFI/BOOT" "$w/esp/reliefos" "$w/esp/leonos" "$w/esp/grub"
printf 'guide\n' > "$w/src/docs/ADVANCED_INSTALL.txt"
for app in desktop settings; do printf 'policy\n' > "$w/out/userland-installer-policy/$app.elf"; done
printf 'canonical runtime\n' > "$w/out/installer/lib/libreliefos.so.2"
printf 'compat runtime\n' > "$w/out/installer/lib/libleonos.so.2"
printf 'gptinit\n' > "$w/out/userland-installer/gptinit.elf"
printf 'installer\n' > "$w/out/userland/installer.elf"
chmod 755 "$w/out/userland/installer.elf"
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
[ -f "$w/stage/usr/lib/reliefos/libreliefos.so.2" ]
[ -f "$w/stage/usr/lib/leonos/libleonos.so.2" ]
[ "$(cat "$w/stage/etc/reliefos/desktop-backend")" = reliefos ]
[ -L "$w/stage/etc/runlevels/default/reliefos-windowd" ]
[ -L "$w/stage/etc/runlevels/default/reliefos-session" ]
[ ! -e "$w/stage/etc/reliefos/xdm.conf" ]
[ ! -e "$w/stage/etc/reliefos/xdm-session" ]
[ ! -e "$w/stage/etc/reliefos/icewm" ]
[ ! -e "$w/stage/etc/reliefos/twmrc" ]
[ ! -e "$w/stage/install/root/$program" ]
[ -f "$w/stage/install/esp/reliefos/loader.elf" ]
[ -f "$w/stage/install/esp/reliefos/kernel.sys" ]
[ -f "$w/stage/install/esp/leonos/kernel.sys" ]
[ -f "$w/stage/install/esp/loader.elf" ]
[ -f "$w/stage/install/esp/grub/grub.cfg" ]
echo 'installer stage: executable, runtime ABI libraries and paired boot layouts are packaged'
