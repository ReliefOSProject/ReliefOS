#!/bin/sh
# Validate real freshly built ELF outputs (never execute target programs).
set -eu
root=${1:?usage: upstream-products.sh UPSTREAM_ROOT}
apps=${2:-$root/../userland}
for entry in libmd/lib/libmd.so.0 libbsd/usr/lib/libbsd.so.0 util-linux/bin/su util-linux/bin/mount util-linux/bin/lsblk util-linux/usr/sbin/fdisk sudo/usr/bin/sudo sudo/usr/lib/sudo/sudoers.so shadow/bin/login shadow/usr/sbin/useradd shadow/usr/sbin/usermod shadow/usr/sbin/userdel exfatprogs/usr/sbin/mkfs.exfat exfatprogs/usr/sbin/fsck.exfat; do
 pkg=${entry%%/*}; path=$root/$pkg/root/${entry#*/}
 test -f "$path"
 readelf -h "$path" | grep -q 'Advanced Micro Devices X86-64'
 case $pkg in exfatprogs)
  if readelf -l -d "$path" | grep -E 'INTERP|\(NEEDED\)'; then echo "not static: $path" >&2; exit 1; fi ;;
 esac
done
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
for package in libbsd libbsd-doc dosfstools dosfstools-doc e2fsprogs e2fsprogs-extra e2fsprogs-libs libcom_err e2fsprogs-doc ncurses ncurses-terminfo; do
 set -- "$root/apk/packages/$package-"[0-9]*.apk
 test "$#" = 1 && test -s "$1"
 "$root/apk/apk.static" --keys-dir "$src/system/rootfs/etc/apk/keys" verify "$1"
done
"$root/apk/apk.static" --version
printf '%s\n' 'upstream production ELF/archive checks: PASS'
for app in busybox pleditor; do
 test -f "$apps/$app.elf"
 readelf -h "$apps/$app.elf" | grep -q 'Advanced Micro Devices X86-64'
 if readelf -d "$apps/$app.elf" | grep -E 'libc\.so\.6|ld-linux'; then exit 1; fi
done
development=$root/runtime-development/usr/lib
for library in libsqlite3.so.0 libz.so.1 libpng16.so.16 libmbedtls.so.21 libmbedx509.so.7 libmbedcrypto.so.16; do
 readelf -d "$development/$library" | grep -q '(SONAME)'
done
sh "$src/tools/build/busybox-check.sh" "$root/busybox/build/.config" "$apps/busybox.links"
if readelf -l -d "$apps/busybox.elf" | grep -E 'INTERP|\(NEEDED\)'; then
    echo "BusyBox is not static: $apps/busybox.elf" >&2; exit 1
fi
printf '%s\n' 'upstream app ELF/SONAME/applet checks: PASS'
