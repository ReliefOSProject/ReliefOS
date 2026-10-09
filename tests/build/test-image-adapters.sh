#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(mktemp -d); trap 'rm -rf "$w"' EXIT HUP INT TERM
${HOSTCC:-cc} -std=c11 -O2 -Wall -Wextra -Werror "$src/tools/host/images/reliefos-ext2-time.c" -lext2fs -lcom_err -o "$w/time"
cat > "$w/reliefos-time" <<EOF
#!/bin/sh
printf 'reliefos\\n' >> '$w/selected-time-tool'
exec '$w/time' "\$@"
EOF
cat > "$w/leonos-time" <<EOF
#!/bin/sh
printf 'leonos\\n' >> '$w/selected-time-tool'
exec '$w/time' "\$@"
EOF
chmod +x "$w/reliefos-time" "$w/leonos-time"
mkdir -p "$w/root/etc" "$w/root/tmp"
mkdir -p "$w/root/etc/reliefos" "$w/root/home/test"
printf 'leonos-standalone-test-v1\n' > "$w/root/etc/reliefos/test-image"
printf 'owned\n' > "$w/root/home/test/file"
chmod 1777 "$w/root/tmp"
printf 'data\n' > "$w/root/file with spaces"
ln -s 'file with spaces' "$w/root/link"
for name in a b; do
    if [ "$name" = a ]; then
        RELIEFOS_EXT2_TIME=$w/reliefos-time LEONOS_EXT2_TIME=$w/leonos-time \
            sh "$src/tools/build/images.sh" ext2 "$w/root" "$w/$name.ext2" 1700000000 5c13543b-732c-4f81-8652-621124484420 > "$w/$name.log" 2>&1
    else
        LEONOS_EXT2_TIME=$w/leonos-time \
            sh "$src/tools/build/images.sh" ext2 "$w/root" "$w/$name.ext2" 1700000000 5c13543b-732c-4f81-8652-621124484420 > "$w/$name.log" 2>&1
    fi
done
[ "$(sed -n '1p' "$w/selected-time-tool")" = reliefos ]
[ "$(sed -n '2p' "$w/selected-time-tool")" = leonos ]
cmp "$w/a.ext2" "$w/b.ext2"
debugfs -R 'stat /tmp' "$w/a.ext2" 2>/dev/null | grep -q '1777'
debugfs -R 'stat /link' "$w/a.ext2" 2>/dev/null | grep -q 'symlink'
debugfs -R 'stat /home/test/file' "$w/a.ext2" 2>/dev/null | grep -Eq 'User: +1000 +Group: +1000'
[ "$(stat -c %a "$w/root/tmp")" = 1777 ]
printf 'image adapters: reproducible ext2, modes and symlinks passed\n'

# Exercise the real ESP staging rule with only GRUB's file-producing command
# mocked. The resulting tree is inspected by the disk and ISO rules.
mkdir -p "$w/fake-bin" "$w/modules"
printf 'loader-payload\n' > "$w/loader.elf"
printf 'kernel-payload\n' > "$w/kernel.sys"
printf 'font-payload\n' > "$w/font.pf2"
printf 'theme=metro\n' > "$w/display.conf"
cat > "$w/fake-bin/grub-mkstandalone" <<EOF
#!/bin/sh
out=
while [ "\$#" -gt 0 ]; do
    if [ "\$1" = -o ]; then out=\$2; shift 2; else shift; fi
done
[ -n "\$out" ] || exit 2
mkdir -p "\$(dirname "\$out")"
printf 'efi-stub\\n' > "\$out"
EOF
chmod +x "$w/fake-bin/grub-mkstandalone"
PATH="$w/fake-bin:$PATH" sh "$src/tools/build/efi-stage.sh" \
    "$src" "$w/modules" "$w/loader.elf" "$w/kernel.sys" "$w/font.pf2" \
    "$w/display.conf" "$w/esp" 1700000000
failures=0
require_file() {
    if [ -s "$1" ]; then printf 'ok - %s\n' "$2"; else printf 'FAIL - %s\n' "$2"; failures=$((failures + 1)); fi
}
require_text() {
    if grep -Fq -- "$2" "$1"; then printf 'ok - %s\n' "$3"; else printf 'FAIL - %s\n' "$3"; failures=$((failures + 1)); fi
}
require_file "$w/esp/reliefos/kernel.sys" 'ESP stages the canonical ReliefOS kernel path'
require_file "$w/esp/reliefos/loader.elf" 'ESP stages the paired canonical ReliefOS loader'
require_file "$w/esp/loader.elf" 'ESP retains a legacy root loader for rollback'
require_file "$w/esp/leonos/kernel.sys" 'ESP keeps a legacy kernel payload for rollback'
if [ -f "$w/esp/reliefos/kernel.sys" ] && [ -f "$w/esp/leonos/kernel.sys" ] &&
    [ -f "$w/esp/reliefos/loader.elf" ] && [ -f "$w/esp/loader.elf" ] &&
    cmp -s "$w/esp/reliefos/kernel.sys" "$w/esp/leonos/kernel.sys" &&
    cmp -s "$w/esp/reliefos/loader.elf" "$w/esp/loader.elf"; then
    printf 'ok - canonical and legacy ESP payloads are matched pairs\n'
else
    printf 'FAIL - canonical and legacy ESP payloads are matched pairs\n'
    failures=$((failures + 1))
fi
for file in boot/grub/grub.cfg boot/grub/installer.cfg boot/grub/live.cfg; do
    require_text "$src/$file" 'module2 /reliefos/kernel.sys reliefos-kernel' "$file uses the canonical module path and tag"
done
require_text "$src/boot/grub/grub.cfg" 'module2 /leonos/kernel.sys leonos-kernel' 'installed GRUB config retains the legacy boot entry'
require_text "$src/boot/grub/grub.cfg" 'multiboot2 /reliefos/loader.elf' 'installed GRUB config pairs the canonical loader and kernel'
require_text "$src/boot/grub/grub.cfg" 'multiboot2 /loader.elf' 'installed GRUB config pairs the legacy loader and kernel'
require_text "$src/boot/grub/installer_embedded.cfg" 'reliefos-installer-iso.marker' 'embedded GRUB discovers the new ISO marker'
require_text "$src/boot/grub/installer_embedded.cfg" 'leonos-installer-iso.marker' 'embedded GRUB still discovers old ISO media'
require_text "$src/tools/build/iso.sh" 'reliefos-installer-iso.marker' 'ISO staging writes the new installer marker'
require_text "$src/tools/build/iso.sh" 'leonos-installer-iso.marker' 'ISO staging keeps the legacy marker for old loaders'
require_text "$src/mk/images.mk" '$(O_IMAGES)/reliefos.vmdk' 'image target uses the ReliefOS VMDK name'
require_text "$src/mk/images.mk" '$(O_IMAGES)/reliefos-live.iso' 'image target uses the ReliefOS live ISO name'
require_text "$src/mk/images.mk" '$(O_IMAGES)/reliefos-installer.iso' 'image target uses the ReliefOS installer ISO name'
require_text "$src/tools/build/disk.sh" 'name=RELIEFOS_ESP' 'GPT ESP partition has the canonical name'
require_text "$src/tools/build/disk.sh" 'name=RELIEFOS_ROOT' 'GPT root partition has the canonical name'
require_text "$src/tools/build/disk.sh" '-n RELIEFOS' 'FAT label fits the eight-character limit'
require_text "$src/tools/build/disk.sh" '41A3EE19-BA85-47A0-9705-A5C128374021' 'published ESP UUID is unchanged'
require_text "$src/tools/build/disk.sh" '5C13543B-732C-4F81-8652-621124484420' 'published root UUID is unchanged'
require_text "$src/userland/apps/installer/install_ops.h" 'TARGET_BOOT "/reliefos/kernel.sys"' 'installer detects the canonical ESP kernel path'
require_text "$src/userland/apps/installer/install_ops.h" 'TARGET_BOOT "/leonos/kernel.sys"' 'installer recognizes a legacy ESP kernel path'
require_text "$src/kernel/reliefnt/boot/loader/main.c" '"/reliefos/kernel.sys"' 'loader prefers the canonical kernel path'
require_text "$src/kernel/reliefnt/boot/loader/main.c" '"/leonos/kernel.sys"' 'loader can boot a legacy kernel path'
require_text "$src/kernel/reliefnt/boot/loader/main.c" '"reliefos-kernel"' 'loader accepts the canonical GRUB kernel tag'
require_text "$src/kernel/reliefnt/boot/loader/main.c" '"leonos-kernel"' 'loader accepts the legacy GRUB kernel tag'
require_text "$src/kernel/reliefnt/drivers/bootstrap/storage/storage_mount.c" '"reliefos-installer-root"' 'kernel accepts the canonical installer root tag'
require_text "$src/kernel/reliefnt/drivers/bootstrap/storage/storage_mount.c" '"leonos-installer-root"' 'kernel accepts the legacy installer root tag'
require_text "$src/kernel/reliefnt/kernel/reliefnt/kernel_debug.c" 'KERNEL_DEBUG_MARKER "RELIEFOS-KDBG-1\n"' 'kernel writes the canonical one-shot marker'
require_text "$src/kernel/reliefnt/boot/loader/main.c" '"/reliefos/state/kerneldebug.next"' 'loader reads the canonical one-shot marker'
require_text "$src/kernel/reliefnt/boot/loader/main.c" '"/leonos/state/kerneldebug.next"' 'loader reads the legacy one-shot marker'
require_text "$src/kernel/reliefnt/drivers/bootstrap/storage/storage_block.c" '"RELIEFOS_ROOT"' 'kernel recognizes the canonical GPT root name'
require_text "$src/kernel/reliefnt/drivers/bootstrap/storage/storage_block.c" '"LEONOS4_ROOT"' 'kernel recognizes the legacy GPT root name'
if [ "$failures" -ne 0 ]; then exit 1; fi

# Exercise installer publication order. A failed GRUB resource copy must leave
# the old configuration and its legacy payload available for the next boot.
mkdir -p "$w/install-source" "$w/rollback-esp/grub" "$w/rollback-esp/leonos" \
    "$w/rollback-esp/reliefos" "$w/rollback-esp/EFI/BOOT"
cp -a "$w/esp/." "$w/install-source/"
printf 'old-grub-config\n' > "$w/rollback-esp/grub/grub.cfg"
printf 'old-legacy-loader\n' > "$w/rollback-esp/loader.elf"
printf 'old-legacy-kernel\n' > "$w/rollback-esp/leonos/kernel.sys"
chmod 555 "$w/rollback-esp/grub"
if sh "$src/userland/storage/leonos-grub-installer" --source "$w/install-source" \
    "$w/rollback-esp" >"$w/grub-installer-fail.log" 2>&1; then
    echo 'FAIL - interrupted installer publication unexpectedly succeeded'
    exit 1
fi
chmod 755 "$w/rollback-esp/grub"
if [ "$(cat "$w/rollback-esp/grub/grub.cfg")" = 'old-grub-config' ] &&
    [ "$(cat "$w/rollback-esp/loader.elf")" = 'old-legacy-loader' ] &&
    [ "$(cat "$w/rollback-esp/leonos/kernel.sys")" = 'old-legacy-kernel' ]; then
    printf 'ok - interrupted publication keeps old GRUB config and rollback payload bootable\n'
else
    printf 'FAIL - interrupted publication damaged the old GRUB rollback entry\n'
    exit 1
fi
sh "$src/userland/storage/leonos-grub-installer" --source "$w/install-source" \
    "$w/rollback-esp" >"$w/grub-installer-success.log"
if cmp -s "$w/install-source/reliefos/kernel.sys" "$w/rollback-esp/reliefos/kernel.sys" &&
    cmp -s "$w/install-source/reliefos/loader.elf" "$w/rollback-esp/reliefos/loader.elf" &&
    [ "$(cat "$w/rollback-esp/grub/grub.cfg")" = "$(cat "$w/install-source/grub/grub.cfg")" ] &&
    [ "$(cat "$w/rollback-esp/loader.elf")" = 'old-legacy-loader' ] &&
    [ "$(cat "$w/rollback-esp/leonos/kernel.sys")" = 'old-legacy-kernel' ]; then
    printf 'ok - successful publication writes matched canonical payload then GRUB config\n'
else
    printf 'FAIL - successful publication did not retain the matched old rollback pair\n'
    exit 1
fi
printf 'image adapters: ReliefOS ESP, ISO, GPT and legacy rollback contracts passed\n'
