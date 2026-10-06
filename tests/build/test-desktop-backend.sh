#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
cd "$repo_root"
work=${TMPDIR:-/tmp}/reliefos-desktop-backend.$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work"

grep -q '^config DESKTOP_BACKEND_RELIEFOS$' Kconfig
grep -q '^config DESKTOP_BACKEND_XORG$' Kconfig
grep -q '^CONFIG_DESKTOP_BACKEND_RELIEFOS=y$' configs/default.conf
! grep -q '^CONFIG_DESKTOP_BACKEND_XORG=y$' configs/default.conf

make -s O="$work/default" defconfig
grep -q '^CONFIG_DESKTOP_BACKEND_RELIEFOS=y$' "$work/default/config/.config"
! grep -q '^CONFIG_DESKTOP_BACKEND_XORG=y$' "$work/default/config/.config"

mkdir -p "$work/xorg/config"
printf '%s\n' 'CONFIG_DESKTOP_BACKEND_XORG=y' > "$work/xorg/config/.config"
make -s O="$work/xorg" olddefconfig
grep -q '^CONFIG_DESKTOP_BACKEND_XORG=y$' "$work/xorg/config/.config"
! grep -q '^CONFIG_DESKTOP_BACKEND_RELIEFOS=y$' "$work/xorg/config/.config"

hostcc=${HOSTCC:-cc}
mkdir -p "$work/src/system" "$work/out"
cp -a system/rootfs "$work/src/system/rootfs"
for name in certs config docs fonts resources test-accounts xorg; do
    ln -s "$repo_root/system/$name" "$work/src/system/$name"
done
for name in configs docs resources test third_party tools userland; do
    ln -s "$repo_root/$name" "$work/src/$name"
done
cp logo.png "$work/src/logo.png"

mkdir -p "$work/out/upstream/sudo/root/usr/bin" \
    "$work/out/upstream/shadow/root/usr/bin" \
    "$work/out/upstream/util-linux/root/bin" \
    "$work/out/pam/root/sbin" \
    "$work/out/sysroot/musl/lib" \
    "$work/out/sysroot/musl/share/licenses/musl" \
    "$work/out/sysroot/musl/share/licenses/mimalloc" \
    "$work/out/system/lib" "$work/out/generated/system" \
    "$work/out/generated/drivers" "$work/out/generated/fonts" \
    "$work/out/userland"
printf '%s\n' fixture > "$work/out/upstream/sudo/root/usr/bin/sudo"
printf '%s\n' fixture > "$work/out/upstream/shadow/root/usr/bin/passwd"
printf '%s\n' fixture > "$work/out/upstream/util-linux/root/bin/su"
printf '%s\n' fixture > "$work/out/pam/root/sbin/unix_chkpwd"
printf '%s\n' fixture > "$work/out/sysroot/musl/lib/libc.so"
printf '%s\n' fixture > "$work/out/sysroot/musl/lib/libmimalloc.so.3"
printf '%s\n' fixture > "$work/out/sysroot/musl/share/licenses/musl/LICENSE"
printf '%s\n' fixture > "$work/out/sysroot/musl/share/licenses/mimalloc/LICENSE"
printf '%s\n' fixture > "$work/out/system/lib/libreliefos.so.2"
legacy_brand=leon
legacy_brand=${legacy_brand}os
printf '%s\n' fixture > "$work/out/system/lib/lib${legacy_brand}.so.2"
printf '%s\n' fixture > "$work/out/generated/system/kerneldebug.sys"
printf '%s\n' fixture > "$work/out/generated/drivers/fixture.drv"
printf '%s\n' fixture > "$work/out/generated/fonts/${legacy_brand}-metro.ttf"
printf '%s\n' fixture > "$work/out/generated/fonts/${legacy_brand}-win95.ttf"
printf '%s\n' fixture > "$work/out/userland/motd.elf"
printf '%s\n' fixture > "$work/out/userland/dynlinkerror.elf"
for locale in $(cat configs/nls/LINGUAS); do
    mkdir -p "$work/out/generated/nls/$locale/LC_MESSAGES"
    printf '%s\n' fixture > "$work/out/generated/nls/$locale/LC_MESSAGES/${legacy_brand}.mo"
done
printf '%s\n' 'component metadata fixture' > "$work/metadata"

"$hostcc" -std=c11 -Wall -Wextra -Werror -Wpedantic -I"$repo_root" \
    "$repo_root/tools/host/manifest/reliefos-stage.c" \
    "$repo_root/tools/host/common/io.c" "$repo_root/tools/host/common/buffer.c" \
    "$repo_root/tools/host/manifest/json.c" -o "$work/stage"
"$hostcc" -std=c11 -Wall -Wextra -Werror -Wpedantic -I"$repo_root" \
    "$repo_root/tools/host/manifest/reliefos-layout.c" -o "$work/layout"

run_rootfs_stage() {
    backend=$1
    config=$work/$backend.config
    dest=$work/$backend-root
    manifest=$work/$backend-manifest.json
    cat > "$config" <<CONFIG
CONFIG_DESKTOP_BACKEND_RELIEFOS=$(test "$backend" = reliefos && printf y || printf n)
CONFIG_DESKTOP_BACKEND_XORG=$(test "$backend" = xorg && printf y || printf n)
CONFIG_RPR_BASE_URL="https://example.invalid/rpr"
CONFIG_VMDK_DEFAULT_LANG="zh_CN.UTF-8"
CONFIG
    sh "$repo_root/tools/build/rootfs-stage.sh" "$work/src" "$work/out" \
        "$config" "$work/metadata" "$work/stage" "$work/layout" \
        "$dest" "$manifest" 1700000000
    printf '%s\n' "$dest"
}

reliefos_root=$(run_rootfs_stage reliefos)
xorg_root=$(run_rootfs_stage xorg)
grep -qx reliefos "$reliefos_root/etc/reliefos/desktop-backend"
grep -qx xorg "$xorg_root/etc/reliefos/desktop-backend"
test -L "$reliefos_root/etc/runlevels/default/reliefos-windowd"
test -L "$reliefos_root/etc/runlevels/default/reliefos-session"
test ! -e "$xorg_root/etc/runlevels/default/reliefos-windowd"
test ! -e "$xorg_root/etc/runlevels/default/reliefos-session"
test -f "$xorg_root/etc/X11/xorg.conf"
test -f "$xorg_root/etc/reliefos/xdm.conf"
test -f "$xorg_root/etc/reliefos/xdm-Xservers"
test -f "$xorg_root/etc/reliefos/xdm-session"
test -x "$xorg_root/usr/lib/reliefos/reliefos-xdm"
test -x "$xorg_root/usr/lib/reliefos/xorg-tty-wrapper"
test -x "$xorg_root/usr/lib/reliefos/xdm-session"
test -f "$xorg_root/etc/pam.d/xdm"
test ! -e "$reliefos_root/etc/X11/xorg.conf"
test ! -e "$reliefos_root/etc/reliefos/xdm.conf"

cat > "$work/invalid.config" <<'CONFIG'
CONFIG_DESKTOP_BACKEND_RELIEFOS=n
CONFIG_DESKTOP_BACKEND_XORG=n
CONFIG_RPR_BASE_URL="https://example.invalid/rpr"
CONFIG_VMDK_DEFAULT_LANG="zh_CN.UTF-8"
CONFIG
if sh "$repo_root/tools/build/rootfs-stage.sh" "$work/src" "$work/out" \
    "$work/invalid.config" "$work/metadata" "$work/stage" "$work/layout" \
    "$work/invalid-root" "$work/invalid-manifest.json" 1700000000 \
    >/dev/null 2>&1; then
    echo 'rootfs-stage accepted an invalid desktop backend' >&2
    exit 1
fi

printf '%s\n' 'ok - desktop backend Kconfig and rootfs policy'
