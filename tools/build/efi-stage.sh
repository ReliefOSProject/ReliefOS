#!/bin/sh
# Build a standalone GRUB EFI application, then assemble the ESP tree.
set -eu
[ "$#" = 7 ] || { echo 'usage: efi-stage SRC MODULES KERNEL FONT DISPLAY STAGE EPOCH' >&2; exit 2; }
src=$1 modules=$2 kernel=$3 font=$4 display=$5 stage=$6 epoch=$7
mkdir -p "$(dirname "$stage")"
work=$(mktemp -d "$stage.new.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/EFI/BOOT" "$work/grub/fonts" "$work/grub/theme" \
    "$work/reliefos/config" "$work/leonos/config"
SOURCE_DATE_EPOCH=$epoch grub-mkstandalone -d "$modules" -O x86_64-efi -o "$work/EFI/BOOT/BOOTX64.EFI" \
 --modules='part_gpt fat iso9660 multiboot2 normal search search_fs_file configfile echo serial terminal video video_bochs video_cirrus efi_gop efi_uga all_video font gfxterm gfxmenu' \
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
find "$work" -exec touch -h -d "@$epoch" {} +
# The private stage is owned by this rule; a previous stage is retained until
# the complete replacement is available.
if [ -d "$stage.previous" ] && [ ! -e "$stage" ]; then mv "$stage.previous" "$stage"; fi
rm -rf "$stage.previous"
if [ -e "$stage" ]; then mv "$stage" "$stage.previous"; fi
mv "$work" "$stage"
rm -rf "$stage.previous"
