#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Check the resolved early binary independently of normal runtime ownership.
set -eu
[ "$#" = 2 ] || exit 2
config=$1 links=$2
for name in sh ash mount umount blkid switch_root modprobe depmod insmod rmmod modinfo uname \
    cat rm mkdir chmod chown sync readlink tr; do
    awk -F / -v name="$name" '$NF==name {found=1} END {exit !found}' "$links" || {
        echo "initramfs BusyBox applet is missing: $name" >&2; exit 1;
    }
done
for symbol in STATIC FEATURE_MODUTILS_ALIAS FEATURE_MODUTILS_SYMBOLS FEATURE_MODPROBE_BLACKLIST FEATURE_CMDLINE_MODULE_OPTIONS; do
    grep -Fqx "CONFIG_$symbol=y" "$config" || {
        echo "initramfs BusyBox requires CONFIG_$symbol" >&2; exit 1;
    }
done
if grep -Eq '^CONFIG_(MODPROBE_SMALL|FEATURE_2_4_MODULES|FEATURE_MTAB_SUPPORT|FEATURE_MOUNT_HELPERS)=y$' "$config"; then
    echo 'initramfs BusyBox enabled incompatible early dependency handling' >&2; exit 1
fi
printf '%s\n' 'initramfs static BusyBox dependency/app applet contract: PASS'
