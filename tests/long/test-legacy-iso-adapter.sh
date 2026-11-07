#!/bin/sh
# The compatibility CLI must use the same real dual-platform ISO producer.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
host=${GRUB_HOST_ROOT:-$src/out/dualboot-build/grub-host}
efi=${GRUB_EFI_DIR:-$src/out/dualboot-build/grub-efi/lib/grub/x86_64-efi}
esp=${BOOT_TEST_ESP:-$src/out/dualboot-build/stage/esp}
work=$(mktemp -d "${TMPDIR:-$src/out/tmp}/legacy-iso.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
truncate -s 32M "$work/root.ext4"
mke2fs -q -F -t ext4 "$work/root.ext4"
python3 "$src/tools/make_installer_iso.py" --out "$work/probe.iso" \
    --stage "$work/esp" --work-dir "$work/work" \
    --kernel "$esp/reliefos/kernel.sys" --grub-font "$esp/grub/fonts/leonos-unicode.pf2" \
    --installer-root "$work/root.ext4" --grub-config "$src/boot/grub/live.cfg" \
    --grub-efi-dir "$efi" --grub-bios-dir "$host/lib/grub/i386-pc" \
    --grub-mkimage "$host/bin/grub-mkimage" \
    --grub-mkstandalone "$host/bin/grub-mkstandalone" > "$work/build.log" 2>&1 || {
    cat "$work/build.log" >&2; exit 1;
}
xorriso -osirrox on -indev "$work/probe.iso" -extract /boot/grub/boot.cat "$work/catalog" \
    > "$work/extract.log" 2>&1
python3 - "$work/catalog" <<'PY'
from pathlib import Path
import sys
data=Path(sys.argv[1]).read_bytes()
assert data[1]==0 and data[32]==0x88 and data[65]==0xef and data[96]==0x88
print('PASS compatibility ISO CLI defaults to real BIOS+UEFI El Torito through shared producer')
PY
cp "$work/probe.iso" "$work/before.iso"
if python3 "$src/tools/make_installer_iso.py" --out "$work/probe.iso" \
    --stage "$work/esp" --work-dir "$work/work" \
    --kernel "$esp/reliefos/kernel.sys" --grub-font "$esp/grub/fonts/leonos-unicode.pf2" \
    --installer-root "$work/root.ext4" --grub-config "$src/boot/grub/live.cfg" \
    --grub-efi-dir "$efi" --grub-bios-dir "$host/lib/grub/i386-pc" \
    --grub-mkimage /bin/false --grub-mkstandalone "$host/bin/grub-mkstandalone" \
    > "$work/failure.log" 2>&1; then
    echo 'FAIL - compatibility ISO CLI ignored a failing BIOS tool' >&2; exit 1
fi
cmp "$work/before.iso" "$work/probe.iso"
printf 'PASS compatibility ISO failure preserves the old published output\n'
