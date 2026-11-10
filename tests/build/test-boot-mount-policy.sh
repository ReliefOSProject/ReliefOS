#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
. "$src/system/initramfs/functions"
work=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-boot-policy.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
uuid=12345678-1234-4567-89ab-123456789abc
printf 'PARTUUID=esp /efi vfat defaults 0 2\n/efi/reliefos/instances/%s /boot none bind 0 0\n' "$uuid" > "$work/fstab"
parse_boot_fstab "$work/fstab" "$uuid"
[ "$esp_source" = PARTUUID=esp ] && [ "$esp_point" = /efi ]
[ "$boot_bind" = "/efi/reliefos/instances/$uuid" ]
if parse_boot_fstab "$work/fstab" 00000000-0000-0000-0000-000000000001; then exit 1; fi
printf 'PARTUUID=legacy /boot vfat defaults 0 2\n' > "$work/fstab"
parse_boot_fstab "$work/fstab" "$uuid"
[ "$esp_point" = /boot ] && [ -z "$boot_bind" ]
for bad in duplicate missing traversal mixed flags; do
    printf 'PARTUUID=esp /efi vfat defaults 0 2\n' > "$work/fstab"
    case $bad in
        duplicate) printf 'PARTUUID=other /boot vfat defaults 0 2\n' >> "$work/fstab" ;;
        traversal) printf '/efi/../outside /boot none bind 0 0\n' >> "$work/fstab" ;;
        mixed) printf 'PARTUUID=esp /boot vfat defaults 0 2\n/efi/reliefos/instances/%s /boot none bind 0 0\n' "$uuid" > "$work/fstab" ;;
        flags) printf '/efi/reliefos/instances/%s /boot none bind,ro 0 0\n' "$uuid" >> "$work/fstab" ;;
    esac
    if parse_boot_fstab "$work/fstab" "$uuid"; then exit 1; fi
done
mkdir "$work/root"
${HOSTCC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror -D_GNU_SOURCE \
    -fsanitize=address,undefined -fno-sanitize-recover=all -ffunction-sections -fdata-sections \
    -I"$src/userland/runtime/include" "$src/tests/host/test_installer_boot_layout.c" \
    -Wl,--gc-sections -o "$work/layout"
"$work/layout" "$work/root"
first=$(RELIEFOS_IMAGE_INSTANCE=first sh "$src/tools/build/image-identity.sh" root)
again=$(RELIEFOS_IMAGE_INSTANCE=first sh "$src/tools/build/image-identity.sh" root)
second=$(RELIEFOS_IMAGE_INSTANCE=second sh "$src/tools/build/image-identity.sh" root)
esp=$(RELIEFOS_IMAGE_INSTANCE=first sh "$src/tools/build/image-identity.sh" esp)
[ "$first" = "$again" ] && [ "$first" != "$second" ] && [ "$first" != "$esp" ]
if RELIEFOS_IMAGE_INSTANCE='../invalid' sh "$src/tools/build/image-identity.sh" root >/dev/null 2>&1; then exit 1; fi
printf 'boot policy: per-root binding, shared ESP, legacy layout, unsafe fstab and deterministic instance IDs passed\n'
