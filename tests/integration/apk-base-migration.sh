#!/bin/sh
# Run against a disposable signed root, on Linux or inside a ReliefOS guest.
# Formatting is restricted to regular files in a newly created private directory.
set -eu
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH
for package in libmd libmd-doc libbsd libbsd-doc dosfstools dosfstools-doc e2fsprogs e2fsprogs-extra e2fsprogs-libs libcom_err e2fsprogs-doc ca-certificates-bundle; do
    apk info -e "$package"
done
test -s /etc/ssl/certs/ca-certificates.crt
apk info --who-owns /etc/ssl/certs/ca-certificates.crt | grep -F 'owned by ca-certificates-bundle-'
for entry in sbin/mkfs.ext4:e2fsprogs sbin/fsck.ext2:e2fsprogs sbin/mkfs.fat:dosfstools usr/lib/libbsd.so.0:libbsd usr/lib/libext2fs.so.2:e2fsprogs-libs; do
    path=${entry%:*} owner=${entry#*:}
    apk info --who-owns "/$path" | grep -F "owned by $owner-"
done
apk info --who-owns /lib/libmd.so.0 | grep -F 'owned by reliefos-auth-support-libraries-'
apk info --who-owns /usr/lib/libmd.so.0 | grep -F 'owned by libmd-'
apk info --who-owns /usr/share/licenses/libmd/COPYING | grep -F 'owned by libmd-doc-'
test "$(readlink /lib/libmd.so.0)" = ../usr/lib/libmd.so.0
for library in libmd.so libmd.so.0.3.0; do
    test "$(readlink "/lib/$library")" = libmd.so.0
done
test ! -e /lib/libmd.a
test ! -e /usr/lib/libmd.a
test ! -e /usr/include/md5.h
apk info --who-owns /usr/lib/libblkid.so.1 | grep -F 'owned by libblkid-'
apk info --who-owns /usr/lib/libuuid.so.1 | grep -F 'owned by libuuid-'
test ! -e /lib/libbsd.so.0
test ! -e /usr/include/bsd
test ! -e /usr/include/ext2fs
test -s /usr/share/licenses/libbsd/COPYING
test -s /usr/share/licenses/e2fsprogs/NOTICE
test -s /usr/share/doc/dosfstools/COPYING
for command in mkfs.ext2 mkfs.ext3 mkfs.ext4 fsck.ext2 fsck.ext3 fsck.ext4 mkfs.fat fsck.fat mkfs.vfat fsck.vfat mkfs.msdos fsck.msdos mkfs.fat32 fsck.fat32 mke2fs e2fsck; do
    test -x "/sbin/$command"
    test "$(readlink "/usr/sbin/$command")" = "../../sbin/$command"
done
for command in chattr lsattr; do
    test -x "/bin/$command"
    test "$(readlink "/usr/bin/$command")" = "../../bin/$command"
done
work=$(mktemp -d /tmp/apk-base-migration.XXXXXX)
trap 'rm -rf "$work"' 0
trap 'exit 130' HUP INT TERM
for filesystem in ext2 ext4; do
    image=$work/$filesystem.img
    truncate -s 64M "$image"
    "/usr/sbin/mkfs.$filesystem" -F -q "$image"
    test "$(blkid -p -s TYPE -o value "$image")" = "$filesystem"
    "/usr/sbin/fsck.$filesystem" -fn "$image"
done
image=$work/fat.img
truncate -s 64M "$image"
/usr/sbin/mkfs.fat32 -F 32 -n APK_TEST "$image"
test "$(blkid -p -s TYPE -o value "$image")" = vfat
test "$(blkid -p -s LABEL -o value "$image")" = APK_TEST
/usr/sbin/fsck.vfat -n "$image"
printf '%s\n' 'PASS: official APK ownership, local dependencies, aliases and ext2/ext4/FAT format/check'
