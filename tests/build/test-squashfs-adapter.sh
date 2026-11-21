#!/bin/sh
# Exercise the SquashFS image adapter without depending on host squashfs-tools.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd -P)
grub-script-check "$src/boot/grub/installer.cfg"
w=$(mktemp -d)
trap 'rm -rf "$w"' EXIT HUP INT TERM
mkdir -p "$w/bin" "$w/stage/root/etc"
printf 'installer data\n' > "$w/stage/root/etc/release"
cat > "$w/bin/mksquashfs" <<'TOOL'
#!/bin/sh
set -eu
[ "$SOURCE_DATE_EPOCH" = 1700000000 ] || {
    echo 'mksquashfs did not receive the image build epoch' >&2
    exit 1
}
for arg do
    case $arg in
        -all-time|-mkfs-time) echo 'mksquashfs received conflicting timestamps' >&2; exit 1 ;;
    esac
done
printf '%s\n' "$@" > "$SQUASHFS_ARGS"
printf 'fixture squashfs\n' > "$2"
TOOL
chmod +x "$w/bin/mksquashfs"
PATH="$w/bin:$PATH" SQUASHFS_ARGS="$w/args" SOURCE_DATE_EPOCH=1600000000 \
    sh "$src/tools/build/images.sh" squashfs "$w/stage" "$w/payload.squashfs" 1700000000
[ "$(cat "$w/payload.squashfs")" = 'fixture squashfs' ]
for option in '-noappend' '-comp' 'gzip' '-b' '131072' '-no-fragments' \
    '-no-xattrs' '-no-exports' '-no-progress' '-processors' '1'; do
    grep -Fxq -- "$option" "$w/args" || {
        echo "missing mksquashfs option: $option" >&2
        exit 1
    }
done
[ ! -e "$w/payload.squashfs.work" ]
echo 'squashfs adapter: deterministic gzip installer payload options are passed'

# The compatibility CLI must resolve its default payload before replacing an
# explicitly selected ESP stage. It must never consume inputs in that tree.
output=$w/build
for file in images/installer-root.ext4 generated/system/kernel.sys \
    generated/system/initramfs.cpio generated/system/initramfs.cpio.json \
    generated/grub/leonos-unicode.pf2 grub-efi/lib/grub/x86_64-efi/kernel.img \
    grub-host/lib/grub/i386-pc/kernel.img grub-host/bin/grub-mkimage \
    grub-host/bin/grub-mkstandalone; do
    mkdir -p "$(dirname "$output/$file")"
    touch "$output/$file"
done
ln -s "$w/stage/root/etc/release" "$output/images/installer-payload.squashfs"
if python3 "$src/tools/make_installer_iso.py" --build-output "$output" \
    --stage "$w/stage" > "$w/iso.log" 2>&1; then
    echo 'compatibility ISO CLI accepted its payload inside the ESP stage' >&2
    exit 1
fi
grep -Fq 'stage must not contain source inputs' "$w/iso.log"
[ "$(cat "$w/stage/root/etc/release")" = 'installer data' ]
echo 'compatibility ISO CLI: default payload is included in source input validation'
