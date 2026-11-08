#!/bin/sh
# Explicit public test-account profile used only by standalone/live images.
set -eu
[ "$#" = 5 ] || exit 2
src=$1 input=$2 output=$3 mode=$4 language=$5
case $mode in live|disk) ;; *) exit 2 ;; esac
case $language in *[!A-Za-z0-9._@-]*|'') echo 'standalone-root: invalid locale name' >&2; exit 2;; esac
mkdir -p "$(dirname "$output")"
work=$(mktemp -d "$output.new.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
cp -a "$input/." "$work/"
rm -f "$work/.apk-complete"
for name in passwd shadow group gshadow; do
    [ ! -L "$work/etc/$name" ] || exit 1
    cp "$src/system/test-accounts/$name" "$work/etc/$name"
    case $name in shadow|gshadow) chmod 600 "$work/etc/$name" ;; *) chmod 644 "$work/etc/$name" ;; esac
done
grep -Fx '%wheel ALL=(ALL:ALL) ALL' "$work/etc/sudoers" >/dev/null
chmod 440 "$work/etc/sudoers"
for name in root home/test; do
    [ ! -L "$work/$name" ] || exit 1
    mkdir -p "$work/$name"
    cp -a "$work/etc/skel/." "$work/$name/"
    chmod 700 "$work/$name"
    # xdg-user-dirs-update initializes special directories at first login.
done
printf 'leonos-standalone-test-v1\n' > "$work/etc/reliefos/test-image"
printf 'test-image=1\n' > "$work/etc/reliefos/installed"
printf 'LANG=%s\nMUSL_LOCPATH=/usr/share/musl/locales\n' "$language" > "$work/etc/reliefos/locale.conf"
if [ "$mode" = disk ]; then
    root_uuid=$(sh "$src/tools/build/image-identity.sh" root)
    esp_uuid=$(sh "$src/tools/build/image-identity.sh" esp)
    cat > "$work/etc/fstab" <<FSTAB
# Stable GPT partition identities owned by tools/build/disk.sh.
/dev/disk/by-partuuid/$root_uuid / ext4 defaults 0 1
/dev/disk/by-partuuid/$esp_uuid /boot vfat defaults 0 2
FSTAB
fi
if [ -d "$output.previous" ] && [ ! -e "$output" ]; then mv "$output.previous" "$output"; fi
rm -rf "$output.previous"
if [ -e "$output" ]; then mv "$output" "$output.previous"; fi
mv "$work" "$output"
rm -rf "$output.previous"
