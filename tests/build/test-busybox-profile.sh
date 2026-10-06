#!/bin/sh
set -eu

src=${1:-$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/source" "$work/build"
git -C "$src/third_party/busybox" archive HEAD | tar -xf - -C "$work/source"
make -s -C "$work/source" O="$work/build" allnoconfig > "$work/config.log" 2>&1
awk 'FNR==NR {
    if ($0 ~ /^CONFIG_/) {split($0,a,"="); value[a[1]]=$0}
    else if ($0 ~ /^# CONFIG_.* is not set$/) value[$2]=$0
    next
}
{key=$1; sub(/=.*/,"",key); if($1=="#") key=$2;
 if(key in value) {print value[key]; delete value[key]} else print}
END {for(key in value) print value[key]}' \
    "$src/userland/busybox/leonos.config" "$work/build/.config" > "$work/build/.config.new"
mv "$work/build/.config.new" "$work/build/.config"
make -s -C "$work/source" O="$work/build" oldconfig </dev/null >> "$work/config.log" 2>&1
make -s -C "$work/source" O="$work/build" HOSTCC="${HOSTCC:-cc}" busybox.links >> "$work/config.log" 2>&1

# Check actual applet output after Kconfig dependencies and choices are resolved.
for name in find xargs dd mount umount fdisk sfdisk blkid lsblk fsck runuser \
    su login passwd sulogin chpasswd adduser addgroup deluser delgroup cryptpw mkpasswd \
    chattr lsattr tune2fs mke2fs mkfs.ext2 mkdosfs mkfs.vfat nologin \
    clear reset resize less wget xxd start-stop-daemon; do
    if awk -F / -v name="$name" '$NF==name {found=1} END {exit !found}' "$work/build/busybox.links"; then
        echo "BusyBox owns externally provided applet: $name" >&2
        exit 1
    fi
done
for name in sh ash false ping ping6 df free top dmesg hexdump killall nc netstat \
    traceroute traceroute6 crond crontab mdev insmod lsmod modprobe rmmod \
    unzip bunzip2 xz unxz cpio bzip2 bc dc tree timeout watch; do
    awk -F / -v name="$name" '$NF==name {found=1} END {exit !found}' "$work/build/busybox.links" || {
        echo "BusyBox applet is missing: $name" >&2
        exit 1
    }
done
if grep -Eq '^CONFIG_(ASH_JOB_CONTROL|HUSH_JOB|FEATURE_WGET_HTTPS|FEATURE_SH_STANDALONE|FEATURE_PREFER_APPLETS)=y$' "$work/build/.config"; then
    echo 'BusyBox enabled an incompatible shell or TLS feature' >&2
    exit 1
fi
printf '%s\n' 'busybox generated profile ownership contract: PASS'
