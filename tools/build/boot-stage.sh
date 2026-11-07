#!/bin/sh
# Assemble shared boot files, the EFI application and the BIOS module inputs.
set -eu
[ "$#" = 9 ] || { echo 'usage: boot-stage SRC EFI_MODULES BIOS_MODULES KERNEL INITRAMFS FONT DISPLAY STAGE EPOCH' >&2; exit 2; }
src=$1 modules=$2 bios=$3 kernel=$4 initramfs=$5 font=$6 display=$7 stage=$8 epoch=$9
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
 --modules='part_gpt fat iso9660 multiboot2 normal cmp search search_fs_file regexp configfile echo serial terminal video video_bochs video_cirrus efi_gop all_video font gfxterm gfxmenu hashsum gcry_sha256 crypto loadenv test' \
 "boot/grub/grub.cfg=$src/boot/grub/embedded.cfg"
: "${RELIEFOS_KERNEL_TOOLS:?set RELIEFOS_KERNEL_TOOLS to the native Kbuild host tool}"
"$RELIEFOS_KERNEL_TOOLS" boot-payload "$kernel" "$initramfs" "$work"
cp "$font" "$work/grub/fonts/leonos-unicode.pf2"
cp "$display" "$work/reliefos/config/display.conf"
cp "$display" "$work/leonos/config/display.conf"
cp "$src/boot/grub/grub.cfg" "$work/grub/grub.cfg"
cp "$src/boot/grub/kernel-slots.cfg" "$work/grub/kernel-slots.cfg"
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
