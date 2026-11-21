#!/bin/sh
# Build the installed and runtime installer roots through the same signed APK
# transaction as the ordinary system, then embed the installed root and the
# minimal ESP payload.  The runtime root is the plain X11 root plus the
# installer programs and the installer-runtime marker; its live session is
# selected by console-session and started by system/xorg/installer-session.
set -eu
. "$(CDPATH= cd -- "$(dirname "$0")/../../scripts" && pwd)/logging.sh"
[ "$#" = 6 ] || [ "$#" = 7 ] || {
    echo 'usage: installer-stage SRC O RAW_ROOT ESP OUTPUT EPOCH [PAYLOAD_STAGE]' >&2
    exit 2
}
src=$1 out=$2 raw=$3 esp=$4 output=$5 epoch=$6
payload=${7:-}
: "${APK_TOOL:?}" "${APK_UPSTREAM:?}" "${APK_OWN_TOOL:?}" "${APK_KEY:?}" "${APK_VERSION:?}"
for path in usr/sbin/reliefos-boot-install usr/sbin/grub-install \
    usr/sbin/grub-bios-setup usr/sbin/grub-probe usr/bin/grub-mkimage \
    usr/lib/grub/i386-pc/boot.img; do
    [ -s "$raw/$path" ] || { echo "installer root is missing required boot tool/data: $path" >&2; exit 1; }
done
mkdir -p "$(dirname "$output")"
work=$(mktemp -d "$output.new.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
cp -a "$raw" "$work/installed-raw"
package_root() {
    reliefos_log APK "$3"
    SOURCE_DATE_EPOCH=$epoch APK_MUSL_SYSROOT=$out/sysroot/musl sh "$src/tools/build/apk-stage.sh" "$src" "$1" "$2" "$3" \
        "$APK_TOOL" "$APK_UPSTREAM" "${APK_DEPS_TOOL:-$out/host/bin/reliefos-deps}" \
        "${APK_LOCK:-$src/configs/dependencies.lock.json}" \
        "$src/configs/apk-ownership.json" "$APK_OWN_TOOL" "$APK_KEY" "$APK_VERSION"
}
package_root "$work/installed-raw" "$work/installed" "$out/packages/apk-installed"
reliefos_log APK 'optional XFCE desktop payload'
sh "$src/tools/build/installer-xfce-profile.sh" "$src" "$work/installed" \
    "$work/xfce-installed" "$out/packages/apk-xfce" "$APK_TOOL" "$APK_UPSTREAM" \
    "${APK_DEPS_TOOL:-$out/host/bin/reliefos-deps}"
cp -a "$work/installed-raw" "$work/console-raw"
sh "$src/tools/build/installer-console-profile.sh" "$work/console-raw"
package_root "$work/console-raw" "$work/console-installed" "$out/packages/apk-console"
for root in "$work/installed" "$work/console-installed"; do
    for path in usr/sbin/reliefos-boot-install usr/sbin/grub-install \
        usr/sbin/grub-bios-setup usr/sbin/grub-probe usr/bin/grub-mkimage \
        usr/lib/grub/i386-pc/boot.img; do
        [ -s "$root/$path" ] || { echo "signed target is missing required boot tool/data: $path" >&2; exit 1; }
    done
done
cp -a "$raw" "$work/runtime-raw"
mkdir -p "$work/runtime-raw/usr/lib/reliefos/apps/installer"
cp "$out/userland/installer.elf" "$work/runtime-raw/usr/lib/reliefos/apps/installer/installer.elf"
chmod 755 "$work/runtime-raw/usr/lib/reliefos/apps/installer/installer.elf"
ln -s ../lib/reliefos/apps/installer/installer.elf "$work/runtime-raw/usr/bin/installer"
mkdir -p "$work/runtime-raw/usr/share/applications"
cp "$src/userland/apps/installer/installer.desktop" \
   "$work/runtime-raw/usr/share/applications/reliefos-installer.desktop"
cp "$src/resources/build-art/app-icons/installer.bmp" \
   "$work/runtime-raw/usr/lib/reliefos/apps/installer/installer.bmp"
for size in 16 32 48 64 128 256; do
    icons=usr/share/icons/hicolor/${size}x${size}/apps
    mkdir -p "$work/runtime-raw/$icons"
    cp "$src/resources/build-art/app-icons/hicolor/${size}x${size}/apps/reliefos-installer.png" "$work/runtime-raw/$icons/"
done
mkdir -p "$work/runtime-raw/usr/lib/reliefos"
cp "$src/system/xorg/installer-session" "$work/runtime-raw/usr/lib/reliefos/installer-session"
chmod 755 "$work/runtime-raw/usr/lib/reliefos/installer-session"
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
cp "$src/Documentation/admin-guide/installation.rst" "$work/runtime-raw/root/installation.rst"
package_root "$work/runtime-raw" "$work/runtime" "$out/packages/apk-installer-runtime"
[ -x "$work/runtime/usr/lib/reliefos/apps/installer/installer.elf" ] || {
    echo 'installer runtime package is missing installer.elf' >&2
    exit 1
}
if [ -n "$payload" ]; then
    mkdir -p "$work/payload"
else
    mkdir -p "$work/runtime/install"
fi
reliefos_log STAGE 'installer payload and ESP'
if [ -n "$payload" ]; then
    mv "$work/installed" "$work/payload/root"
    mv "$work/console-installed" "$work/payload/root-console"
    mv "$work/xfce-installed" "$work/payload/root-xfce"
    cp -a "$esp" "$work/payload/esp"
else
    mv "$work/installed" "$work/runtime/install/root"
    mv "$work/console-installed" "$work/runtime/install/root-console"
    mv "$work/xfce-installed" "$work/runtime/install/root-xfce"
    cp -a "$esp" "$work/runtime/install/esp"
fi
"${INSTALLER_DEDUP_TOOL:-$out/host/bin/reliefos-dedup}" "$work/runtime"
find "$work/runtime" -exec touch -h -d "@$epoch" {} +
if [ -d "$output.previous" ] && [ ! -e "$output" ]; then mv "$output.previous" "$output"; fi
rm -rf "$output.previous"
if [ -e "$output" ]; then mv "$output" "$output.previous"; fi
mv "$work/runtime" "$output"
rm -rf "$output.previous"
if [ -n "$payload" ]; then
    mkdir -p "$(dirname "$payload")"
    find "$work/payload" -exec touch -h -d "@$epoch" {} +
    if [ -d "$payload.previous" ] && [ ! -e "$payload" ]; then
        mv "$payload.previous" "$payload"
    fi
    rm -rf "$payload.previous"
    if [ -e "$payload" ]; then mv "$payload" "$payload.previous"; fi
    mv "$work/payload" "$payload"
    rm -rf "$payload.previous"
fi
