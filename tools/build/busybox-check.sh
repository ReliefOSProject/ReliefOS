#!/bin/sh
# Validate the resolved profile, not just requested Kconfig settings.
set -eu
[ "$#" = 2 ] || { echo 'usage: busybox-check.sh CONFIG LINKS' >&2; exit 2; }
config=$1 links=$2
awk '
$0 !~ /^\/(bin|sbin|usr\/bin|usr\/sbin)\/[^/[:space:]]+$/ ||
$0 ~ /\/(\.|\.\.|busybox)$/ {print "unsafe BusyBox link: " $0 > "/dev/stderr"; bad=1}
seen[$0]++ {print "duplicate BusyBox link: " $0 > "/dev/stderr"; bad=1}
END {exit bad}' "$links"
for name in find xargs dd mount umount fdisk sfdisk blkid lsblk fsck runuser \
    su login passwd sulogin chpasswd adduser addgroup deluser delgroup \
    cryptpw mkpasswd chattr lsattr tune2fs mke2fs mkfs.ext2 mkdosfs mkfs.vfat nologin \
    ping ping6 clear reset resize less wget xxd start-stop-daemon; do
    if awk -F / -v name="$name" '$NF==name {found=1} END {exit !found}' "$links"; then
        echo "BusyBox owns externally provided applet: $name" >&2
        exit 1
    fi
done
for name in sh ash false df free top dmesg hexdump killall nc netstat \
    traceroute traceroute6 crond crontab mdev insmod lsmod modprobe rmmod \
    unzip bunzip2 xz unxz cpio bzip2 bc dc tree timeout watch; do
    awk -F / -v name="$name" '$NF==name {found=1} END {exit !found}' "$links" || {
        echo "BusyBox applet is missing: $name" >&2
        exit 1
    }
done
for path in /bin/sh /bin/ash /bin/false; do
    grep -Fqx "$path" "$links" || { echo "BusyBox shell link is missing: $path" >&2; exit 1; }
done
if grep -Eq '^CONFIG_(HUSH|ASH_JOB_CONTROL|HUSH_JOB|SSL_CLIENT|TLS|FEATURE_WGET_HTTPS|FEATURE_SH_STANDALONE|FEATURE_SH_NOFORK|FEATURE_PREFER_APPLETS)=y$' "$config"; then
    echo 'BusyBox enabled an incompatible shell or TLS feature' >&2
    exit 1
fi
printf '%s\n' 'busybox generated profile ownership contract: PASS'
