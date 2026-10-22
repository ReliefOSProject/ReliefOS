#!/bin/sh
# Dual El Torito live/installer ISO; upstream tools own all disk formats.
set -eu
. "$(CDPATH= cd -- "$(dirname "$0")/../../scripts" && pwd)/logging.sh"
[ "$#" = 10 ] || { echo 'usage: iso SRC EFI_MODULES ESP ROOT CONFIG OUTPUT EPOCH VOLUME BIOS_MODULES GRUB_MKIMAGE' >&2; exit 2; }
src=$1 modules=$2 esp=$3 root=$4 config=$5 output=$6 epoch=$7 volume=$8
bios=$9 mkimage=${10}
export SOURCE_DATE_EPOCH=$epoch TZ=UTC LC_ALL=C
[ ! -L "$output" ] && { [ ! -e "$output" ] || [ -f "$output" ]; } || {
    echo "refusing unsafe ISO output: $output" >&2; exit 1;
}
mkdir -p "$(dirname "$output")"
work=$(mktemp -d "$output.work.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
cp -a "$esp" "$work/tree"
mkdir -p "$work/tree/install" "$work/tree/boot/grub" "$work/efi/EFI/BOOT"
"${GRUB_MKSTANDALONE:-grub-mkstandalone}" -d "$modules" -O x86_64-efi -o "$work/efi/EFI/BOOT/BOOTX64.EFI" \
 --modules='part_gpt fat iso9660 multiboot2 normal cmp search search_fs_file configfile echo serial terminal video video_bochs video_cirrus efi_gop efi_uga all_video font gfxterm gfxmenu' \
 "boot/grub/grub.cfg=$src/boot/grub/installer_embedded.cfg"
"$mkimage" -O i386-pc-eltorito -d "$bios" -p /grub \
    -c "$src/boot/grub/installer_embedded.cfg" -o "$work/tree/boot/grub/bios.img" \
    $(cat "$src/boot/grub/bios-modules.list")
cp "$work/efi/EFI/BOOT/BOOTX64.EFI" "$work/tree/EFI/BOOT/BOOTX64.EFI"
cp "$root" "$work/tree/install/root.fat"
cp "$config" "$work/tree/grub/grub.cfg"
printf 'ReliefOS installer ISO volume\n' > "$work/tree/reliefos-installer-iso.marker"
# Old embedded GRUB releases still locate the installer media by this marker.
printf 'ReliefOS installer ISO volume\n' > "$work/tree/leonos-installer-iso.marker"
find "$work/efi" "$work/tree" -exec touch -h -d "@$epoch" {} +
size=$(( ($(stat -c %s "$work/efi/EFI/BOOT/BOOTX64.EFI") + 1048575) / 1048576 + 8 ))
[ "$size" -ge 16 ] || size=16
truncate -s "${size}M" "$work/tree/boot/efiboot.img"
mkfs.fat --invariant -F 16 -n RELIEFOS "$work/tree/boot/efiboot.img"
mcopy -s -m -i "$work/tree/boot/efiboot.img" "$work/efi/EFI" ::/
touch -d "@$epoch" "$work/tree/boot/efiboot.img"
reliefos_log ISO "$output"
sh "$src/tools/build/run-logged.sh" --tag XORRISO "${output%.iso}.xorriso.log" \
 xorriso -as mkisofs -iso-level 3 -R -J -V "$volume" \
 -uid 0 -gid 0 --set_all_file_dates "$(date -u -d "@$epoch" +%Y%m%d%H%M%S)00" \
 --modification-date="$(date -u -d "@$epoch" +%Y%m%d%H%M%S)00" \
 -b boot/grub/bios.img -c boot/grub/boot.cat -no-emul-boot -boot-load-size 4 \
 -boot-info-table --grub2-boot-info -eltorito-alt-boot \
 -e boot/efiboot.img -no-emul-boot -o "$work/output.iso" "$work/tree"
mv "$work/output.iso" "$output"
