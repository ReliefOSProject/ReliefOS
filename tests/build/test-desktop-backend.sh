#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
cd "$repo_root"
work=${TMPDIR:-/tmp}/reliefos-desktop-backend.$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work"

# The Motif X11 frontend is the only desktop frontend; the backend choice is
# gone from the configuration.
if grep -q 'DESKTOP_BACKEND' Kconfig configs/default.conf; then
    echo 'desktop backend choice is still configurable' >&2
    exit 1
fi

make -s O="$work/default" defconfig
if grep -q 'DESKTOP_BACKEND' "$work/default/config/.config"; then
    echo 'defconfig still emits desktop backend symbols' >&2
    exit 1
fi

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
printf 'busybox\ttool\t1\t1\t0\t0\t0\tBusyBox\tTools\t\t0\n' > "$work/metadata"
printf 'ncurses\tlibrary\t1\t1\t0\t1\t0\tncurses\tLibraries\t\t0\n' >> "$work/metadata"
mkdir -p "$work/out/upstream/ncurses/root/usr/bin" \
    "$work/out/upstream/ncurses/root/usr/share/terminfo" \
    "$work/out/upstream/ncurses/root/usr/include" \
    "$work/out/upstream/fixture-provider/root/usr/bin" \
    "$work/out/upstream/fixture-provider/root/usr/sbin" \
    "$work/out/upstream/inactive/root/usr/bin"
printf 'ncurses clear\n' > "$work/out/upstream/ncurses/root/usr/bin/clear"
ln -s clear "$work/out/upstream/ncurses/root/usr/bin/reset"
printf 'terminal data\n' > "$work/out/upstream/ncurses/root/usr/share/terminfo/fixture"
printf 'development header\n' > "$work/out/upstream/ncurses/root/usr/include/curses.h"
printf 'selected provider\n' > "$work/out/upstream/fixture-provider/root/usr/bin/hexdump"
printf 'external ping provider\n' > "$work/out/upstream/fixture-provider/root/usr/bin/ping"
printf 'storage provider\n' > "$work/out/upstream/fixture-provider/root/usr/sbin/mkfs.ext4"
printf 'inactive provider\n' > "$work/out/upstream/inactive/root/usr/bin/tree"
printf 'BusyBox fixture\n' > "$work/out/userland/busybox.elf"
printf '%s\n' /bin/sh /bin/ash /bin/false /bin/ping /bin/ping6 /bin/hexdump \
    /usr/bin/clear /usr/bin/reset /sbin/mkfs.ext4 /bin/passwd /usr/bin/tree > "$work/out/userland/busybox.links"

"$hostcc" -std=c11 -Wall -Wextra -Werror -Wpedantic -I"$repo_root" \
    "$repo_root/tools/host/manifest/reliefos-stage.c" \
    "$repo_root/tools/host/common/io.c" "$repo_root/tools/host/common/buffer.c" \
    "$repo_root/tools/host/manifest/json.c" -o "$work/stage"
"$hostcc" -std=c11 -Wall -Wextra -Werror -Wpedantic -I"$repo_root" \
    "$repo_root/tools/host/manifest/reliefos-layout.c" -o "$work/layout"

cat > "$work/rootfs.config" <<CONFIG
CONFIG_RPR_BASE_URL="https://example.invalid/rpr"
CONFIG_VMDK_DEFAULT_LANG="zh_CN.UTF-8"
CONFIG
ROOTFS_UPSTREAM_PACKAGES='sudo shadow util-linux ncurses fixture-provider' \
sh "$repo_root/tools/build/rootfs-stage.sh" "$work/src" "$work/out" \
    "$work/rootfs.config" "$work/metadata" "$work/stage" "$work/layout" \
    "$work/root" "$work/manifest.json" 1700000000
xorg_root=$work/root

grep -qx xorg "$xorg_root/etc/reliefos/desktop-backend"
test ! -e "$xorg_root/etc/runlevels/default/reliefos-windowd"
test ! -e "$xorg_root/etc/runlevels/default/reliefos-session"
test -f "$xorg_root/etc/X11/xorg.conf"
# Mouse0 must consume the kernel's authoritative absolute position.
# xf86-input-evdev drops the absolute axes of a mixed rel/abs mouse
# ("ignoring absolute axes") and rebuilds the cursor from the relative deltas,
# which permanently drifts away from the kernel position whenever a motion
# event is lost (the VMware mouse offset bug).  IgnoreRelativeAxes "true" is
# not enough: the scrollwheel probe branch re-enables EVDEV_RELATIVE_EVENTS and
# the relative path wins.  The trinary value IgnoreAbsoluteAxes "false" means
# "unignore": both axis classes initialize and the absolute position wins every
# sync frame.
mouse0=$(awk '/Identifier "Mouse0"/,/^EndSection/' "$xorg_root/etc/X11/xorg.conf")
if ! printf '%s\n' "$mouse0" | grep -q 'Option "IgnoreAbsoluteAxes" "false"'; then
    echo 'Mouse0 must unignore the absolute axes (IgnoreAbsoluteAxes "false")' >&2
    echo 'FAIL - xorg.conf mouse absolute position contract' >&2
    exit 1
fi
test -f "$xorg_root/etc/reliefos/xdm.conf"
test -f "$xorg_root/etc/reliefos/xdm-Xservers"
test -f "$xorg_root/etc/reliefos/xdm-session"
test -x "$xorg_root/usr/lib/reliefos/reliefos-xdm"
test -x "$xorg_root/usr/lib/reliefos/xorg-tty-wrapper"
test -x "$xorg_root/usr/lib/reliefos/xdm-session"
test -f "$xorg_root/etc/pam.d/xdm"
# IceWM is the only window manager: its configuration is staged and no old
# window manager file may survive in the product root.
test -f "$xorg_root/etc/reliefos/icewm/preferences"
test -f "$xorg_root/etc/reliefos/icewm/menu"
test -f "$xorg_root/etc/reliefos/icewm/keys"
test -f "$xorg_root/etc/reliefos/icewm/themes/light/default.theme"
test ! -e "$xorg_root/etc/reliefos/twmrc"
test ! -e "$xorg_root/usr/bin/twm"
test ! -e "$xorg_root/usr/bin/twm.real"
test -f "$xorg_root/bin/busybox"
test -L "$xorg_root/bin/sh"
test -L "$xorg_root/bin/ash"
test -L "$xorg_root/bin/false"
test -L "$xorg_root/bin/ping6"
test "$(readlink "$xorg_root/bin/ping6")" = busybox
# Commands claimed by a staged upstream root keep their provider and never
# grow a BusyBox link, even under a different directory.
cmp "$work/out/upstream/fixture-provider/root/usr/bin/hexdump" "$xorg_root/usr/bin/hexdump"
cmp "$work/out/upstream/fixture-provider/root/usr/bin/ping" "$xorg_root/usr/bin/ping"
cmp "$work/out/upstream/fixture-provider/root/usr/sbin/mkfs.ext4" "$xorg_root/usr/sbin/mkfs.ext4"
test ! -e "$xorg_root/bin/hexdump"
test ! -e "$xorg_root/bin/ping"
# /sbin/mkfs.ext4 stays the storage compatibility alias to the upstream
# provider; BusyBox must not claim the command.
test -L "$xorg_root/sbin/mkfs.ext4"
test "$(readlink "$xorg_root/sbin/mkfs.ext4")" = ../usr/sbin/mkfs.ext4
cmp "$work/out/upstream/ncurses/root/usr/bin/clear" "$xorg_root/usr/bin/clear"
test -L "$xorg_root/usr/bin/reset"
test "$(readlink "$xorg_root/usr/bin/reset")" = clear
test -f "$xorg_root/usr/share/terminfo/fixture"
test -f "$xorg_root/etc/terminfo/fixture"
test ! -e "$xorg_root/usr/include/curses.h"
cmp "$work/out/upstream/shadow/root/usr/bin/passwd" "$xorg_root/usr/bin/passwd"
test ! -e "$xorg_root/bin/passwd"
# Roots outside ROOTFS_UPSTREAM_PACKAGES never reach the rootfs: the command
# stays a generated BusyBox link instead of the inactive provider file.
test -L "$xorg_root/usr/bin/tree"
test "$(readlink "$xorg_root/usr/bin/tree")" = ../../bin/busybox

printf '%s\n' 'ok - X11 session policy and upstream staging'
