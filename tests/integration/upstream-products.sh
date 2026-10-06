#!/bin/sh
# Validate real freshly built ELF outputs (never execute target programs).
set -eu
root=${1:?usage: upstream-products.sh UPSTREAM_ROOT}
for entry in libmd/lib/libmd.so.0 libbsd/lib/libbsd.so.0 util-linux/bin/su util-linux/bin/mount util-linux/bin/lsblk util-linux/usr/sbin/fdisk sudo/usr/bin/sudo sudo/usr/lib/sudo/sudoers.so shadow/bin/login shadow/usr/sbin/useradd shadow/usr/sbin/usermod shadow/usr/sbin/userdel e2fsprogs/usr/sbin/mkfs.ext4 e2fsprogs/usr/sbin/fsck.ext4 e2fsprogs/usr/sbin/mkfs.ext2 e2fsprogs/usr/sbin/fsck.ext2 dosfstools/usr/sbin/mkfs.fat dosfstools/usr/sbin/fsck.fat exfatprogs/usr/sbin/mkfs.exfat exfatprogs/usr/sbin/fsck.exfat; do
 pkg=${entry%%/*}; path=$root/$pkg/root/${entry#*/}
 test -f "$path"
 readelf -h "$path" | grep -q 'Advanced Micro Devices X86-64'
 case $pkg in e2fsprogs|dosfstools|exfatprogs)
  if readelf -l -d "$path" | grep -E 'INTERP|\(NEEDED\)'; then echo "not static: $path" >&2; exit 1; fi ;;
 esac
done
for archive in libncursesw.a libtinfow.a; do
 test -s "$root/ncurses/root/usr/lib/$archive"
 ar t "$root/ncurses/root/usr/lib/$archive" | grep -q '\.o$'
done
"$root/apk/apk.static" --version
printf '%s\n' 'upstream production ELF/archive checks: PASS'
apps=${2:-$root/userland}
for app in busybox sl pleditor; do
 test -f "$apps/$app.elf"
 readelf -h "$apps/$app.elf" | grep -q 'Advanced Micro Devices X86-64'
 if readelf -d "$apps/$app.elf" | grep -E 'libc\.so\.6|ld-linux'; then exit 1; fi
done
for library in sqlite.so.3; do
 readelf -d "$apps/$library" | grep -q '(SONAME)'
done
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
sh "$src/tools/build/busybox-check.sh" "$root/busybox/build/.config" "$apps/busybox.links"
if readelf -l -d "$apps/busybox.elf" | grep -E 'INTERP|\(NEEDED\)'; then
    echo "BusyBox is not static: $apps/busybox.elf" >&2; exit 1
fi
printf '%s\n' 'upstream app ELF/SONAME/applet checks: PASS'
