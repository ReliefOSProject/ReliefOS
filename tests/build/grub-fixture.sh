# Fixtures for build-graph tests only; runtime tests use the actual GRUB tools.
# The .mod input has the real ELF32 container type, without runtime exports.
grub_fixture_initramfs() {
    python3 - "$1" "$2" <<'PY'
from pathlib import Path
import hashlib, json, sys
kernel, archive = map(Path, sys.argv[1:])
archive.write_bytes(b'fixture newc carrier\n')
abi='0123456789ab'+'0'*52
Path(str(archive)+'.json').write_text(json.dumps({'schema':1,'release':'5.1.2-ko1-'+abi[:12],
 'module_abi':abi,'kernel_sha256':hashlib.sha256(kernel.read_bytes()).hexdigest(),
 'initramfs_sha256':hashlib.sha256(archive.read_bytes()).hexdigest()}))
PY
}

grub_fixture_efi() {
    fixture_efi=$1
    mkdir -p "$fixture_efi"
    printf 'int grub_fixture_input;\n' | ${HOSTCC:-cc} -m64 -ffreestanding -x c -c -o "$fixture_efi/input.o" -
    for fixture_name in part_gpt fat iso9660 multiboot2 normal cmp search search_fs_file regexp configfile echo serial terminal video video_bochs video_cirrus efi_gop all_video font gfxterm gfxmenu hashsum gcry_sha256 crypto loadenv test; do
        cp "$fixture_efi/input.o" "$fixture_efi/$fixture_name.mod"
    done
    printf 'fixture EFI kernel\n' > "$fixture_efi/kernel.img"
    rm "$fixture_efi/input.o"
}

grub_fixture_bios() {
    fixture_bios=$1
    mkdir -p "$fixture_bios"
    printf 'int grub_fixture_input;\n' | ${HOSTCC:-cc} -m32 -ffreestanding -x c -c -o "$fixture_bios/input.o" -
    for fixture_name in $(cat "$src/boot/grub/bios-modules.list"); do
        cp "$fixture_bios/input.o" "$fixture_bios/$fixture_name.mod"
    done
    for fixture_name in boot.img diskboot.img cdboot.img kernel.img moddep.lst; do
        printf 'fixture BIOS input\n' > "$fixture_bios/$fixture_name"
    done
    rm "$fixture_bios/input.o"
}
