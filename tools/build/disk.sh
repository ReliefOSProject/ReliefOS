#!/bin/sh
# Rootless GPT/FAT/ext4 disk composition through upstream format tools.
set -eu
[ "$#" = 8 ] || { echo 'usage: disk ESP ROOT_EXT4 OUTPUT_RAW OUTPUT_VMDK EPOCH BIOS_MODULES GRUB_MKIMAGE GRUB_BIOS_SETUP' >&2; exit 2; }
esp=$1 root=$2 raw=$3 vmdk=$4 epoch=$5
bios=$6 mkimage=$7 setup=$8
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
export SOURCE_DATE_EPOCH=$epoch TZ=UTC LC_ALL=C
[ "$raw" != "$vmdk" ] || { echo 'raw and VMDK outputs must differ' >&2; exit 2; }
[ ! "$raw" -ef "$vmdk" ] || { echo 'raw and VMDK outputs alias one file' >&2; exit 2; }
[ -f "$root" ] && [ -s "$root" ] || { echo 'root input must be a nonempty regular image' >&2; exit 1; }
root_bytes=$(stat -c %s "$root")
[ "$((root_bytes % 512))" = 0 ] || { echo 'root image must contain whole sectors' >&2; exit 1; }
for output in "$raw" "$vmdk"; do
    [ ! "$root" -ef "$output" ] || { echo 'output aliases the root input' >&2; exit 1; }
    [ ! -L "$output" ] && { [ ! -e "$output" ] || [ -f "$output" ]; } || {
        echo "refusing unsafe image output: $output" >&2; exit 1;
    }
done
for name in boot.img diskboot.img kernel.img lzma_decompress.img moddep.lst; do
    [ -s "$bios/$name" ] || { echo "missing BIOS input: $name" >&2; exit 1; }
done
[ -x "$mkimage" ] && [ -x "$setup" ] || { echo 'missing locked BIOS tools' >&2; exit 1; }
mkdir -p "$(dirname "$raw")" "$(dirname "$vmdk")"
[ "$(stat -c %d "$(dirname "$raw")")" = "$(stat -c %d "$(dirname "$vmdk")")" ] || {
    echo 'image pair publication requires one filesystem' >&2; exit 1;
}
work=$(mktemp -d "$raw.work.XXXXXX")
raw_saved=0 vmdk_saved=0 raw_published=0 vmdk_published=0 committed=0
cleanup() {
    result=$?
    trap - EXIT HUP INT TERM
    if [ "$committed" = 0 ]; then
        if [ "$raw_published" = 1 ]; then
            if [ "$raw_saved" = 1 ]; then mv -f "$work/old.raw" "$raw" || echo 'failed to restore previous raw image' >&2
            else rm -f "$raw"; fi
        fi
        if [ "$vmdk_published" = 1 ]; then
            if [ "$vmdk_saved" = 1 ]; then mv -f "$work/old.vmdk" "$vmdk" || echo 'failed to restore previous VMDK' >&2
            else rm -f "$vmdk"; fi
        fi
    fi
    rm -rf "$work"
    exit "$result"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cp -a "$esp" "$work/esp"
mkdir -p "$work/esp/reliefos/config"
printf 'set reliefos_root=PARTUUID=5c13543b-732c-4f81-8652-621124484420\n' > "$work/esp/reliefos/config/root.cfg"
printf 'version=1\nbios=1\nuefi=1\n' > "$work/esp/reliefos/config/bootloaders.conf"
marker=reliefos/config/esp-41a3ee19-ba85-47a0-9705-a5c128374021.marker
printf 'ReliefOS ESP identity\n' > "$work/esp/$marker"
cat > "$work/bios.cfg" <<CONFIG
# GRUB executes embedded config with its simple rescue parser.
set root=
search --no-floppy --file /$marker --set=root
set prefix=(\$root)/grub
configfile (\$root)/grub/grub.cfg
CONFIG
find "$work/esp" -exec touch -h -d "@$epoch" {} +
esp_mib=$(( ($(du -sb "$work/esp" | cut -f1) + 33554431) / 33554432 * 32 + 32 ))
[ "$esp_mib" -ge 128 ] || esp_mib=128
root_sectors=$((root_bytes / 512))
esp_sectors=$((esp_mib * 2048))
root_start=$((4096 + esp_sectors))
total_sectors=$((root_start + root_sectors + 2048))
minimum=${IMAGE_MIN_MIB:-0}
case $minimum in ''|*[!0-9]*) echo 'IMAGE_MIN_MIB must be an integer' >&2; exit 2;; esac
if [ "$((minimum * 2048))" -gt "$total_sectors" ]; then total_sectors=$((minimum * 2048)); fi
truncate -s "$((total_sectors * 512))" "$work/disk.raw"
sfdisk "$work/disk.raw" <<TABLE
label: gpt
label-id: B4A70D6A-278B-4A2C-986B-D79B8044C840
unit: sectors

start=2048,size=2048,type=21686148-6449-6E6F-744E-656564454649,uuid=7C2A9C78-48C7-49E0-A057-9821ECA5C029,name=RELIEFOS_BIOS
start=4096,size=$esp_sectors,type=U,uuid=41A3EE19-BA85-47A0-9705-A5C128374021,name=RELIEFOS_ESP
start=$root_start,size=$root_sectors,type=L,uuid=5C13543B-732C-4F81-8652-621124484420,name=RELIEFOS_ROOT
TABLE
truncate -s "${esp_mib}M" "$work/esp.fat"
mkfs.fat --invariant -F 32 -s 2 -n RELIEFOS "$work/esp.fat"
for entry in "$work/esp"/*; do mcopy -s -m -i "$work/esp.fat" "$entry" ::/; done
dd if="$work/esp.fat" of="$work/disk.raw" bs=512 seek=4096 conv=notrunc status=none
dd if="$root" of="$work/disk.raw" bs=512 seek="$root_start" conv=notrunc status=none
cp "$bios/boot.img" "$work/boot.img"
"$mkimage" -d "$bios" -O i386-pc -p /grub -c "$work/bios.cfg" \
    -o "$work/core.img" $(cat "$src/boot/grub/bios-modules.list")
printf '(hd0) %s\n' "$work/disk.raw" > "$work/device.map"
"$setup" -s --root-device=hd0,gpt2 -d "$work" -m "$work/device.map" "$work/disk.raw"
sfdisk --verify "$work/disk.raw"
qemu-img convert -f raw -O vmdk "$work/disk.raw" "$work/disk.vmdk"
qemu-img check -f vmdk "$work/disk.vmdk"
# Two POSIX renames cannot be power-failure atomic. Retain old inodes so
# ordinary publication errors/signals restore the prior complete pair.
if [ -e "$raw" ]; then ln "$raw" "$work/old.raw"; raw_saved=1; fi
if [ -e "$vmdk" ]; then ln "$vmdk" "$work/old.vmdk"; vmdk_saved=1; fi
raw_published=1
mv "$work/disk.raw" "$raw"
vmdk_published=1
mv "$work/disk.vmdk" "$vmdk"
committed=1
