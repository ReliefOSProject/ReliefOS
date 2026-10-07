#!/bin/sh
# Build both installer policy roots through the same signed APK transaction as
# the ordinary system, then embed the installed root and minimal ESP payload.
set -eu
. "$(CDPATH= cd -- "$(dirname "$0")/../../scripts" && pwd)/logging.sh"
[ "$#" = 6 ] || { echo 'usage: installer-stage SRC O RAW_ROOT ESP OUTPUT EPOCH' >&2; exit 2; }
src=$1 out=$2 raw=$3 esp=$4 output=$5 epoch=$6
: "${APK_TOOL:?}" "${APK_UPSTREAM:?}" "${APK_OWN_TOOL:?}" "${APK_KEY:?}" "${APK_VERSION:?}"
mkdir -p "$(dirname "$output")"
work=$(mktemp -d "$output.new.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
cp -a "$raw" "$work/installed-raw"
for app in desktop settings; do
    mkdir -p "$work/installed-raw/usr/lib/reliefos/apps/$app"
    cp "$out/userland-installer-policy/$app.elf" "$work/installed-raw/usr/lib/reliefos/apps/$app/$app.elf"
done
mkdir -p "$work/installed-raw/usr/lib/reliefos"
cp "$out/installer/lib/libreliefos.so.2" "$work/installed-raw/usr/lib/reliefos/libreliefos.so.2"
cp "$out/installer/lib/libleonos.so.2" "$work/installed-raw/usr/lib/leonos/libleonos.so.2"
rm -f "$work/installed-raw/etc/license.conf" "$work/installed-raw/etc/install.id"
package_root() {
    reliefos_log APK "$3"
    SOURCE_DATE_EPOCH=$epoch APK_MUSL_SYSROOT=$out/sysroot/musl sh "$src/tools/build/apk-stage.sh" "$src" "$1" "$2" "$3" \
        "$APK_TOOL" "$APK_UPSTREAM" "${APK_DEPS_TOOL:-$out/host/bin/reliefos-deps}" \
        "${APK_LOCK:-$src/configs/dependencies.lock.json}" \
        "$src/configs/apk-ownership.json" "$APK_OWN_TOOL" "$APK_KEY" "$APK_VERSION"
}
restore_native_desktop_policy() {
    runtime_root=$1
    mkdir -p "$runtime_root/etc/reliefos" "$runtime_root/etc/runlevels/default"
    printf 'reliefos\n' > "$runtime_root/etc/reliefos/desktop-backend"
    for service in reliefos-windowd reliefos-session; do
        rm -f "$runtime_root/etc/runlevels/default/$service"
        ln -s "../../init.d/$service" "$runtime_root/etc/runlevels/default/$service"
    done
    rm -f "$runtime_root/etc/reliefos/xdm.conf" "$runtime_root/etc/reliefos/xdm-Xservers" \
        "$runtime_root/etc/X11/xorg.conf" "$runtime_root/etc/pam.d/xdm" \
        "$runtime_root/etc/reliefos/xdm-session" \
        "$runtime_root/usr/lib/reliefos/reliefos-xdm" "$runtime_root/usr/lib/reliefos/xdm-session" \
        "$runtime_root/usr/lib/reliefos/xorg-tty-wrapper"
    rm -rf "$runtime_root/etc/reliefos/icewm"
}
package_root "$work/installed-raw" "$work/installed" "$out/packages/apk-installed"
cp -a "$raw" "$work/runtime-raw"
restore_native_desktop_policy "$work/runtime-raw"
mkdir -p "$work/runtime-raw/usr/lib/reliefos/apps/installer"
cp "$out/userland/installer.elf" "$work/runtime-raw/usr/lib/reliefos/apps/installer/installer.elf"
chmod 755 "$work/runtime-raw/usr/lib/reliefos/apps/installer/installer.elf"
ln -s ../lib/reliefos/apps/installer/installer.elf "$work/runtime-raw/usr/bin/installer"
mkdir -p "$work/runtime-raw/usr/lib/reliefos"
cp "$out/installer/lib/libreliefos.so.2" "$work/runtime-raw/usr/lib/reliefos/libreliefos.so.2"
cp "$out/installer/lib/libleonos.so.2" "$work/runtime-raw/usr/lib/leonos/libleonos.so.2"
mkdir -p "$work/runtime-raw/usr/lib/reliefos/apps/gptinit" "$work/runtime-raw/root" "$work/runtime-raw/etc/reliefos"
cp "$out/userland-installer/gptinit.elf" "$work/runtime-raw/usr/lib/reliefos/apps/gptinit/gptinit.elf"
cat > "$work/runtime-raw/usr/lib/reliefos/apps/gptinit/manifest.ini" <<'MANIFEST'
[app]
id=gptinit
name=GPT initializer
version=installer
category=Installer tools
exec=gptinit.elf
entry=0
terminal=1
hidden=1
commands=gptinit
MANIFEST
ln -s ../lib/reliefos/apps/gptinit/gptinit.elf "$work/runtime-raw/usr/bin/gptinit"
printf 'installer\n' > "$work/runtime-raw/etc/reliefos/installer-runtime"
cp "$src/docs/ADVANCED_INSTALL.txt" "$work/runtime-raw/root/ADVANCED_INSTALL.txt"
package_root "$work/runtime-raw" "$work/runtime" "$out/packages/apk-installer-runtime"
[ -x "$work/runtime/usr/lib/reliefos/apps/installer/installer.elf" ] || {
    echo 'installer runtime package is missing installer.elf' >&2
    exit 1
}
mkdir -p "$work/runtime/install"
reliefos_log STAGE 'installer payload and ESP'
mv "$work/installed" "$work/runtime/install/root"
cp -a "$esp" "$work/runtime/install/esp"
"${INSTALLER_DEDUP_TOOL:-$out/host/bin/reliefos-dedup}" "$work/runtime"
find "$work/runtime" -exec touch -h -d "@$epoch" {} +
if [ -d "$output.previous" ] && [ ! -e "$output" ]; then mv "$output.previous" "$output"; fi
rm -rf "$output.previous"
if [ -e "$output" ]; then mv "$output" "$output.previous"; fi
mv "$work/runtime" "$output"
rm -rf "$output.previous"
