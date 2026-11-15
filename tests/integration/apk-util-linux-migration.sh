#!/bin/sh
# Run as root in a disposable signed root or ReliefOS guest. Only regular
# files in a new private directory are partitioned here; no device is written.
set -eu
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH
test "$(id -u)" = 0
for package in util-linux util-linux-misc util-linux-login runuser lsblk sfdisk blkid \
    mount umount setarch libblkid libuuid libmount libfdisk libsmartcols liblastlog2 \
    libeconf utmps-libs skalibs-libs util-linux-doc util-linux-login-doc; do
    apk info -e "$package"
done
for entry in bin/mount:mount bin/umount:umount bin/lsblk:lsblk sbin/sfdisk:sfdisk \
    sbin/blkid:blkid sbin/fdisk:util-linux-misc bin/su:util-linux-login sbin/runuser:runuser \
    bin/login:reliefos-authentication usr/bin/newgrp:reliefos-authentication \
    sbin/nologin:reliefos-authentication etc/pam.d/su-l:reliefos-authentication \
    usr/lib/libblkid.so.1:libblkid usr/lib/libuuid.so.1:libuuid; do
    path=${entry%:*} owner=${entry#*:}
    apk info --who-owns "/$path" | grep -F "owned by $owner-"
done
for command in fdisk sfdisk runuser fsck blkid; do
    test "$(readlink "/usr/sbin/$command")" = "../../sbin/$command"
    test -x "/sbin/$command"
done
for command in mount umount lsblk; do
    test "$(readlink "/usr/bin/$command")" = "../../bin/$command"
    test -x "/bin/$command"
done
test "$(stat -c %a /bin/su)" = 4755
test "$(stat -c %u /bin/su)" = 0
# The source archive/database must retain the original root:5 setgid wall.
test "$(stat -c '%u:%g:%a' /usr/bin/wall)" = 0:5:2755
for path in usr/include/blkid usr/include/uuid usr/include/libeconf.h usr/lib/libblkid.a \
    usr/lib/libuuid.a usr/lib/libeconf.a; do test ! -e "/$path"; done
for command in fdisk sfdisk lsblk blkid mount umount su runuser; do
    "$command" --version | grep -F 'util-linux'
done
work=$(mktemp -d /tmp/apk-util-linux.XXXXXX)
trap 'rm -rf "$work"' 0
trap 'exit 130' HUP INT TERM
truncate -s 64M "$work/gpt.img"
printf 'label: gpt\n,24M,U\n,,L\n' | sfdisk --no-reread --no-tell-kernel "$work/gpt.img"
sfdisk --verify "$work/gpt.img"
sfdisk --json "$work/gpt.img" >"$work/partitions.json"
grep -Fi 'C12A7328-F81F-11D2-BA4B-00A0C93EC93B' "$work/partitions.json"
grep -Fi '0FC63DAF-8483-4772-8E79-3D69D8477DE4' "$work/partitions.json"
fdisk -l "$work/gpt.img"
test "$(runuser -u nobody -- id -u)" = "$(id -u nobody)"
test "$(su -s /bin/sh nobody -c 'id -u')" = "$(id -u nobody)"
# An unprivileged runuser must fail, without relying on a password prompt.
runuser -u nobody -- /bin/sh -c 'if runuser -u root -- id; then exit 1; fi'
printf '%s\n' 'PASS: official util-linux packages, ownership, aliases, GPT files and PAM identities'
