#!/bin/sh
# Assemble shared boot files, the EFI application and the BIOS module inputs.
set -eu
[ "$#" = 8 ] || { echo 'usage: boot-stage SRC EFI_MODULES BIOS_MODULES KERNEL FONT DISPLAY STAGE EPOCH' >&2; exit 2; }
src=$1 modules=$2 bios=$3 kernel=$4 font=$5 display=$6 stage=$7 epoch=$8
export SOURCE_DATE_EPOCH=$epoch TZ=UTC LC_ALL=C
for name in $(cat "$src/boot/grub/bios-modules.list"); do
    [ -s "$bios/$name.mod" ] || { echo "missing required GRUB BIOS input: $name.mod" >&2; exit 1; }
done
for name in boot.img diskboot.img cdboot.img kernel.img moddep.lst; do
    [ -s "$bios/$name" ] || { echo "missing required GRUB BIOS input: $name" >&2; exit 1; }
done
mkdir -p "$(dirname "$stage")"
work=$(mktemp -d "$stage.new.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/EFI/BOOT" "$work/grub/fonts" "$work/grub/theme" \
    "$work/reliefos/config" "$work/leonos/config" "$work/grub/i386-pc"
"${GRUB_MKSTANDALONE:-grub-mkstandalone}" -d "$modules" -O x86_64-efi -o "$work/EFI/BOOT/BOOTX64.EFI" \
 --modules='part_gpt fat iso9660 multiboot2 normal cmp search search_fs_file regexp configfile echo serial terminal video video_bochs video_cirrus efi_gop efi_uga all_video font gfxterm gfxmenu' \
 "boot/grub/grub.cfg=$src/boot/grub/embedded.cfg"
cp "$kernel" "$work/reliefos/kernel.sys"
# New images keep a byte-identical legacy payload so a prior GRUB menu entry
# remains bootable until a successful installer update publishes its config.
cp "$kernel" "$work/leonos/kernel.sys"
cp "$font" "$work/grub/fonts/leonos-unicode.pf2"
cp "$display" "$work/reliefos/config/display.conf"
cp "$display" "$work/leonos/config/display.conf"
cp "$src/boot/grub/grub.cfg" "$work/grub/grub.cfg"
cp "$src/boot/grub/theme/theme.txt" "$work/grub/theme/theme.txt"
cp -a "$bios/." "$work/grub/i386-pc/"
find "$work" -exec touch -h -d "@$epoch" {} +
# The private stage is owned by this rule; a previous stage is retained until
# the complete replacement is available.
if [ -d "$stage.previous" ] && [ ! -e "$stage" ]; then mv "$stage.previous" "$stage"; fi
rm -rf "$stage.previous"
if [ -e "$stage" ]; then mv "$stage" "$stage.previous"; fi
mv "$work" "$stage"
rm -rf "$stage.previous"
