#!/bin/sh
# Exercise the real upstream BIOS installer on an owned regular-file disk.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
grub=${GRUB_HOST_ROOT:-$src/out/dualboot-build/grub-host}
modules=$grub/lib/grub/i386-pc
for name in grub-mkimage grub-bios-setup grub-mkstandalone grub-probe; do
    [ -x "$grub/bin/$name" ] || { echo "missing locked BIOS tool: $name; run make grub-host" >&2; exit 1; }
    "$grub/bin/$name" --version | grep -q '2\.12' || exit 1
done
for name in boot.img diskboot.img cdboot.img kernel.img biosdisk.mod part_gpt.mod fat.mod multiboot2.mod normal.mod iso9660.mod vbe.mod; do
    [ -s "$modules/$name" ] || { echo "missing BIOS image input: $name" >&2; exit 1; }
done
# The PC decompressor jumps to the first loaded byte, not the ELF e_entry.
# Host CFLAGS must not move compiler-generated cold sections before startup.S.
python3 - "$modules/kernel.img" <<'PY'
import struct
import sys

data = open(sys.argv[1], 'rb').read()
assert data[:6] == b'\x7fELF\x01\x01', 'expected little-endian ELF32 GRUB core'
entry, = struct.unpack_from('<I', data, 24)
offset, = struct.unpack_from('<I', data, 32)
stride, count = struct.unpack_from('<HH', data, 46)
starts = [struct.unpack_from('<I', data, offset + i * stride + 12)[0]
          for i in range(count)
          if struct.unpack_from('<I', data, offset + i * stride + 8)[0] & 6 == 6]
assert starts and entry == min(starts), 'GRUB PC entry does not start executable payload'
print('PASS GRUB BIOS core entry starts executable payload')
PY
work=$(mktemp -d "${TMPDIR:-$src/out/tmp}/grub-bios-test.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
truncate -s 192M "$work/disk.raw"
sfdisk "$work/disk.raw" >"$work/sfdisk.log" <<'TABLE'
label: gpt
unit: sectors

start=2048,size=2048,type=21686148-6449-6E6F-744E-656564454649
start=4096,size=262144,type=U
start=266240,type=L
TABLE
cp --sparse=always "$work/disk.raw" "$work/before.raw"
cp "$modules/boot.img" "$work/boot.img"
"$grub/bin/grub-mkimage" -d "$modules" -O i386-pc -p /grub \
    -o "$work/core.img" biosdisk part_gpt fat normal multiboot2
printf '(hd0) %s\n' "$work/disk.raw" > "$work/device.map"
"$grub/bin/grub-bios-setup" -s --root-device=hd0,gpt2 -d "$work" -m "$work/device.map" "$work/disk.raw" >"$work/setup.log" 2>&1 || {
    cat "$work/setup.log" >&2; exit 1;
}
# Only MBR boot code and the BIOS Boot partition may change. GPT, partition
# table bytes in the protective MBR, ESP and root must remain byte-identical.
dd if="$work/before.raw" of="$work/mbr.before" bs=1 skip=440 count=72 status=none
dd if="$work/disk.raw" of="$work/mbr.after" bs=1 skip=440 count=72 status=none
cmp "$work/mbr.before" "$work/mbr.after"
cmp -i 512 -n $((2048 * 512 - 512)) "$work/before.raw" "$work/disk.raw"
cmp -i $((4096 * 512)) "$work/before.raw" "$work/disk.raw"
if cmp -s -n 440 "$work/before.raw" "$work/disk.raw"; then
    echo 'BIOS setup did not publish MBR boot code' >&2; exit 1
fi
if cmp -s -i $((2048 * 512)) -n $((2048 * 512)) "$work/before.raw" "$work/disk.raw"; then
    echo 'BIOS setup did not embed the core image' >&2; exit 1
fi
sfdisk --verify "$work/disk.raw"
cp --sparse=always "$work/disk.raw" "$work/installed.raw"
cp --sparse=always "$work/before.raw" "$work/other.raw"
printf '(hd1) %s\n' "$work/other.raw" >> "$work/device.map"
if "$grub/bin/grub-bios-setup" -s --root-device=hd1,gpt2 -d "$work" -m "$work/device.map" "$work/disk.raw" >"$work/wrong-root.log" 2>&1; then
    echo 'explicit root on another disk was accepted' >&2; exit 1
fi
cmp "$work/installed.raw" "$work/disk.raw"
cmp "$work/before.raw" "$work/other.raw"
if "$grub/bin/grub-bios-setup" --root-device=hd0,gpt2 -d "$work" -m "$work/device.map" "$work/disk.raw" >"$work/probe-root.log" 2>&1; then
    echo 'explicit root without skip-fs-probe was accepted' >&2; exit 1
fi
cmp "$work/installed.raw" "$work/disk.raw"
printf 'PASS GRUB BIOS: real 2.12 tools, rootless GPT embedding, unchanged GPT/ESP/root\n'
