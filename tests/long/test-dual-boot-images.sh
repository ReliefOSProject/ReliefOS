#!/bin/sh
# Real format/GRUB tools, owned sparse files only; this is not a guest-boot test.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
grub=${GRUB_HOST_ROOT:-$src/out/dualboot-build/grub-host}
esp=${BOOT_TEST_ESP:-$src/out/dualboot-build/stage/esp}
work=$(mktemp -d "${TMPDIR:-$src/out/tmp}/dual-images.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir "$work/esp"
cp -a "$esp/." "$work/esp/"
truncate -s 32M "$work/root.ext4"
mke2fs -q -F -t ext4 -b 4096 -U 5c13543b-732c-4f81-8652-621124484420 "$work/root.ext4"
sh "$src/tools/build/disk.sh" "$work/esp" "$work/root.ext4" \
    "$work/disk.raw" "$work/disk.vmdk" 1700000000 \
    "$grub/lib/grub/i386-pc" "$grub/bin/grub-mkimage" "$grub/bin/grub-bios-setup" > "$work/disk.log" 2>&1 || {
    cat "$work/disk.log" >&2; exit 1;
}
GRUB_MKSTANDALONE=$grub/bin/grub-mkstandalone sh "$src/tools/build/iso.sh" \
    "$src" "${GRUB_EFI_DIR:-$src/out/dualboot-build/grub-efi/lib/grub/x86_64-efi}" "$work/esp" "$work/root.ext4" \
    "$src/boot/grub/live.cfg" "$work/live.iso" 1700000000 RELIEFOSTEST \
    "$grub/lib/grub/i386-pc" "$grub/bin/grub-mkimage" > "$work/iso.log" 2>&1 || {
    cat "$work/iso.log" >&2; exit 1;
}
python3 - "$work" <<'PY'
from pathlib import Path
import json
import struct
import subprocess
import sys

w = Path(sys.argv[1])
raw = w / 'disk.raw'
table = json.loads(subprocess.check_output(['sfdisk', '--json', raw]))['partitiontable']
parts = table['partitions']
assert len(parts) == 3
assert (parts[0]['start'], parts[0]['size']) == (2048, 2048)
assert parts[0]['type'].lower() == '21686148-6449-6e6f-744e-656564454649'
assert parts[1]['start'] == 4096
assert parts[1]['uuid'].lower() == '41a3ee19-ba85-47a0-9705-a5c128374021'
assert parts[2]['uuid'].lower() == '5c13543b-732c-4f81-8652-621124484420'
assert parts[2]['start'] == parts[1]['start'] + parts[1]['size']
with raw.open('rb') as stream:
    mbr = stream.read(512)
    assert any(mbr[:440]) and mbr[510:512] == b'\x55\xaa'
    stream.seek(2048 * 512)
    assert any(stream.read(2048 * 512))
subprocess.run(['sfdisk', '--verify', raw], check=True)
subprocess.run(['qemu-img', 'convert', '-f', 'vmdk', '-O', 'raw', w/'disk.vmdk', w/'roundtrip.raw'], check=True)
subprocess.run(['cmp', raw, w/'roundtrip.raw'], check=True)
subprocess.run(['mcopy', '-i', f'{raw}@@{parts[1]["start"] * 512}',
                '::/reliefos/kernel.sys', str(w/'raw-kernel')], check=True)
subprocess.run(['cmp', w/'esp/reliefos/kernel.sys', w/'raw-kernel'], check=True)
for inside, outside in [('/boot/grub/boot.cat', 'catalog'),
                         ('/reliefos/kernel.sys', 'iso-kernel'), ('/install/root.fat', 'iso-root')]:
    subprocess.run(['xorriso', '-osirrox', 'on', '-indev', str(w/'live.iso'),
                    '-extract', inside, str(w/outside)], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
catalog = (w/'catalog').read_bytes()
assert catalog[0] == 1 and catalog[1] == 0
assert sum(struct.unpack('<16H', catalog[:32])) & 0xffff == 0
assert catalog[30:32] == b'\x55\xaa' and catalog[32] == 0x88
assert struct.unpack_from('<H', catalog, 38)[0] == 4
assert catalog[64] in (0x90, 0x91) and catalog[65] == 0xef and catalog[96] == 0x88
subprocess.run(['cmp', w/'esp/reliefos/kernel.sys', w/'iso-kernel'], check=True)
subprocess.run(['cmp', w/'root.ext4', w/'iso-root'], check=True)
print('PASS real dual images: GPT/MBR, unchanged role UUIDs, VMDK roundtrip, BIOS+UEFI El Torito and identical kernel/root payloads')
PY
# Failed tools must not replace already published media.
cp "$work/live.iso" "$work/live.before"
if GRUB_MKSTANDALONE=$grub/bin/grub-mkstandalone sh "$src/tools/build/iso.sh" \
    "$src" "${GRUB_EFI_DIR:-$src/out/dualboot-build/grub-efi/lib/grub/x86_64-efi}" "$work/esp" "$work/root.ext4" \
    "$src/boot/grub/live.cfg" "$work/live.iso" 1700000000 RELIEFOSTEST \
    "$grub/lib/grub/i386-pc" /bin/false > "$work/iso-fail.log" 2>&1; then
    echo 'FAIL - ISO accepted a failing BIOS tool' >&2; exit 1
fi
cmp "$work/live.before" "$work/live.iso"
printf 'raw sentinel\n' > "$work/protected.raw"
printf 'vmdk sentinel\n' > "$work/protected.vmdk"
if sh "$src/tools/build/disk.sh" "$work/esp" "$work/root.ext4" \
    "$work/protected.raw" "$work/protected.vmdk" 1700000000 \
    "$grub/lib/grub/i386-pc" "$grub/bin/grub-mkimage" /bin/false > "$work/disk-fail.log" 2>&1; then
    echo 'FAIL - disk accepted failing BIOS setup' >&2; exit 1
fi
[ "$(cat "$work/protected.raw")" = 'raw sentinel' ]
[ "$(cat "$work/protected.vmdk")" = 'vmdk sentinel' ]
printf 'PASS dual image failure publication preserves prior outputs\n'
# Force the second publication rename to fail, after the raw rename succeeds.
mkdir "$work/bin"
cat > "$work/bin/mv" <<'MV'
#!/bin/sh
if [ "$#" = 2 ] && [ "${1##*/}" = disk.vmdk ]; then exit 73; fi
exec /usr/bin/mv "$@"
MV
chmod +x "$work/bin/mv"
if PATH="$work/bin:$PATH" sh "$src/tools/build/disk.sh" "$work/esp" "$work/root.ext4" \
    "$work/protected.raw" "$work/protected.vmdk" 1700000000 \
    "$grub/lib/grub/i386-pc" "$grub/bin/grub-mkimage" "$grub/bin/grub-bios-setup" > "$work/rename-fail.log" 2>&1; then
    echo 'FAIL - disk publication accepted a failed rename' >&2; exit 1
fi
[ "$(cat "$work/protected.raw")" = 'raw sentinel' ]
[ "$(cat "$work/protected.vmdk")" = 'vmdk sentinel' ]
# Refuse malformed root images and alias outputs before publication.
printf 'partial sector\n' > "$work/short.root"
if sh "$src/tools/build/disk.sh" "$work/esp" "$work/short.root" \
    "$work/protected.raw" "$work/protected.vmdk" 1700000000 \
    "$grub/lib/grub/i386-pc" "$grub/bin/grub-mkimage" "$grub/bin/grub-bios-setup" > "$work/root-fail.log" 2>&1; then
    echo 'FAIL - disk accepted a partial-sector root' >&2; exit 1
fi
ln "$work/root.ext4" "$work/root-alias.raw"
if sh "$src/tools/build/disk.sh" "$work/esp" "$work/root.ext4" \
    "$work/root-alias.raw" "$work/protected.vmdk" 1700000000 \
    "$grub/lib/grub/i386-pc" "$grub/bin/grub-mkimage" "$grub/bin/grub-bios-setup" > "$work/alias-fail.log" 2>&1; then
    echo 'FAIL - disk accepted an output aliasing its root input' >&2; exit 1
fi
# The image adapter grows ESP before placing root, without a fixed root offset.
truncate -s 80M "$work/esp/large-resource"
sh "$src/tools/build/disk.sh" "$work/esp" "$work/root.ext4" \
    "$work/grown.raw" "$work/grown.vmdk" 1700000000 \
    "$grub/lib/grub/i386-pc" "$grub/bin/grub-mkimage" "$grub/bin/grub-bios-setup" > "$work/grown.log" 2>&1
python3 - "$work/grown.raw" <<'PY'
import json, subprocess, sys
parts=json.loads(subprocess.check_output(['sfdisk','--json',sys.argv[1]]))['partitiontable']['partitions']
assert parts[1]['size'] > 128*2048
assert parts[2]['start'] == parts[1]['start'] + parts[1]['size']
PY
printf 'PASS grown ESP, malformed root, input/output alias and pair-publication rollback\n'
