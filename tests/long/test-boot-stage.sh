#!/bin/sh
# Inspect the actual firmware payloads and failed-publication boundary.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
. "$src/tests/build/grub-fixture.sh"
RELIEFOS_KERNEL_TOOLS=${RELIEFOS_KERNEL_TOOLS:-$src/out/kbuild-os-native/host/bin/reliefos-kernel-tools}
export RELIEFOS_KERNEL_TOOLS
grub=${GRUB_HOST_ROOT:-$src/out/dualboot-build/grub-host}
work=$(mktemp -d "${TMPDIR:-$src/out/tmp}/boot-stage-test.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
printf 'kernel\n' > "$work/kernel.sys"
grub_fixture_initramfs "$work/kernel.sys" "$work/initramfs.cpio"
printf 'font\n' > "$work/font.pf2"
printf 'theme=metro\n' > "$work/display.conf"
GRUB_MKSTANDALONE=$grub/bin/grub-mkstandalone sh "$src/tools/build/boot-stage.sh" \
    "$src" "${GRUB_EFI_DIR:-$src/out/dualboot-build/grub-efi/lib/grub/x86_64-efi}" "$grub/lib/grub/i386-pc" \
    "$work/kernel.sys" "$work/initramfs.cpio" "$work/font.pf2" "$work/display.conf" "$work/esp" 1700000000
cmp "$work/kernel.sys" "$work/esp/reliefos/kernel.sys"
cmp "$work/kernel.sys" "$work/esp/leonos/kernel.sys"
cmp "$grub/lib/grub/i386-pc/normal.mod" "$work/esp/grub/i386-pc/normal.mod"
python3 - "$work/esp" <<'PY'
from pathlib import Path
import struct
import sys
root = Path(sys.argv[1])
image = (root / 'EFI/BOOT/BOOTX64.EFI').read_bytes()
assert image[:2] == b'MZ'
pe = struct.unpack_from('<I', image, 60)[0]
assert image[pe:pe+4] == b'PE\0\0'
assert struct.unpack_from('<H', image, pe+4)[0] == 0x8664
assert struct.unpack_from('<H', image, pe+24)[0] == 0x20b
assert struct.unpack_from('<H', image, pe+24+68)[0] == 10
module = (root / 'grub/i386-pc/normal.mod').read_bytes()
assert module[:5] == b'\x7fELF\x01'
assert struct.unpack_from('<H', module, 18)[0] == 3
PY
cp -a "$work/esp" "$work/before"
mkdir "$work/missing-modules"
if GRUB_MKSTANDALONE=$grub/bin/grub-mkstandalone sh "$src/tools/build/boot-stage.sh" \
    "$src" "${GRUB_EFI_DIR:-$src/out/dualboot-build/grub-efi/lib/grub/x86_64-efi}" "$work/missing-modules" \
    "$work/kernel.sys" "$work/initramfs.cpio" "$work/font.pf2" "$work/display.conf" "$work/esp" 1700000000 \
    >"$work/failure.log" 2>&1; then
    echo 'missing BIOS modules unexpectedly published a stage' >&2; exit 1
fi
diff -qr "$work/before" "$work/esp"
printf 'PASS boot stage: EFI PE, BIOS ELF modules, matched kernels, failed publication preserved\n'
