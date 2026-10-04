#!/bin/sh
# Compose one private root from an explicit plan; the C helper validates every
# destination and publishes a complete JSON manifest, including overlay owners.
set -eu
[ "$#" = 9 ] || { echo 'usage: rootfs-stage SRC O CONFIG METADATA STAGE_TOOL LAYOUT_TOOL DEST MANIFEST EPOCH' >&2; exit 2; }
src=$1 out=$2 config=$3 metadata=$4 stage_tool=$5 layout_tool=$6 dest=$7 manifest=$8 epoch=$9
reliefos_enabled=$(grep -Fxc 'CONFIG_DESKTOP_BACKEND_RELIEFOS=y' "$config" || true)
xorg_enabled=$(grep -Fxc 'CONFIG_DESKTOP_BACKEND_XORG=y' "$config" || true)
case "$reliefos_enabled:$xorg_enabled" in
    1:0) desktop_backend=reliefos ;;
    0:1) desktop_backend=xorg ;;
    *)
        echo 'rootfs-stage: exactly one desktop backend must be enabled' >&2
        exit 2
        ;;
esac
mkdir -p "$(dirname "$dest")"
work=$(mktemp -d "$dest.new.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir "$work/root" "$work/data" "$work/manifests"
plan=$work/plan
: > "$plan"
# Plan columns: kind, source, guest path, mode, component, overlay policy and
# the optional guest gid (decimal, default 0; see reliefos-stage read_plan).
record() { printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" "$5" "$6" "${7:-0}" >> "$plan"; }
file() { record f "$1" "/$2" "${3:-0644}" "${4:-reliefos-base}" "${5:-unique}" "${6:-0}"; }
tree() { record t "$1" "/$2" 0755 "$3" "${4:-unique}"; }
link() { record l "$2" "/$1" 0777 "${3:-reliefos-base}" "${4:-unique}"; }
enabled() { awk -F '\t' -v id="$1" '$1==id && $4==1 {found=1} END {exit !found}' "$metadata"; }
# Independent upstream installations never share a destination during builds.
for package in libmd libbsd util-linux sudo shadow e2fsprogs dosfstools exfatprogs; do
    for directory in bin sbin lib usr etc; do
        input=$out/upstream/$package/root/$directory
        if [ -d "$input" ]; then tree "$input" "$directory" "$package"; fi
    done
done
for spec in auth pam; do
    for directory in lib usr etc sbin bin; do
        input=$out/$spec/root/$directory
        if [ -d "$input" ]; then tree "$input" "$directory" "$spec"; fi
    done
done
# Product account/PAM policy intentionally overrides vendor example files.
tree "$src/system/rootfs" '' product-policy override
printf '%s\n' "$desktop_backend" > "$work/data/desktop-backend"
file "$work/data/desktop-backend" etc/reliefos/desktop-backend 0644 product-policy override
if [ "$desktop_backend" = xorg ]; then
    for service in reliefos-windowd reliefos-session; do
        record x - "/etc/runlevels/default/$service" 0000 product-policy override
    done
    file "$src/system/xorg/xorg.conf" etc/X11/xorg.conf 0644 product-policy override
    file "$src/system/xorg/reliefos-xdm" usr/lib/reliefos/reliefos-xdm 0755 reliefos-apps override
    file "$src/system/xorg/xorg-tty-wrapper" usr/lib/reliefos/xorg-tty-wrapper 0755 reliefos-apps override
    file "$src/system/xorg/xdm.conf" etc/reliefos/xdm.conf 0644 product-policy override
    file "$src/system/xorg/xdm-Xservers" etc/reliefos/xdm-Xservers 0644 product-policy override
    file "$src/system/xorg/xdm-session" etc/reliefos/xdm-session 0755 reliefos-apps override
    file "$src/system/xorg/xdm-session" usr/lib/reliefos/xdm-session 0755 reliefos-apps override
    file "$src/system/xorg/twmrc" etc/reliefos/twmrc 0644 product-policy override
    file "$src/system/xorg/wallpaper.png" etc/reliefos/wallpaper.png 0644 product-policy override
    file "$src/system/xorg/pam-xdm" etc/pam.d/xdm 0644 product-policy override
fi
legacy=$src/system/rootfs/var/lib/leonos/users.db
if [ -e "$legacy" ] || [ -L "$legacy" ]; then
    [ ! -L "$legacy" ] && [ "$(od -An -tx1 "$legacy" | tr -d ' \n')" = 3253554100000000 ] || { echo 'populated legacy account seed requires recovery' >&2; exit 1; }
    record x - /var/lib/leonos/users.db 0000 product-policy override
fi
"$layout_tool" >> "$plan"
for directory in desktop documents downloads; do
    record d - "/etc/skel/$directory" 0700 product-policy override
done
for spec in 'etc/sudoers.d 0750' 'var/lib/sudo 0700' 'run/sudo 0711' 'run/sudo/ts 0700'; do
    set -- $spec; record d - "/$1" "$2" product-policy override
done
for name in shadow gshadow; do file "$src/system/rootfs/etc/$name" "etc/$name" 0600 product-policy override; done
file "$src/system/rootfs/etc/sudoers" etc/sudoers 0440 product-policy override
# Regular program modes remain explicit even when copied from a tool's stage.
for spec in 'sudo usr/bin/sudo' 'shadow usr/bin/passwd' 'util-linux bin/su'; do
    set -- $spec; file "$out/upstream/$1/root/$2" "$2" 4755 authentication override
done
file "$out/pam/root/sbin/unix_chkpwd" sbin/unix_chkpwd 4755 authentication override
file "$out/sysroot/musl/lib/libc.so" lib/ld-musl-x86_64.so.1 0755 musl
file "$out/sysroot/musl/lib/libc.so" lib/libc.so 0755 musl
file "$out/sysroot/musl/lib/libmimalloc.so.3" lib/libmimalloc.so.3 0755 reliefos-mimalloc
file "$out/system/lib/libreliefos.so.2" usr/lib/reliefos/libreliefos.so.2 0755 reliefos-apps
file "$out/system/lib/libleonos.so.2" usr/lib/leonos/libleonos.so.2 0755 reliefos-apps
for package in musl mimalloc; do tree "$out/sysroot/musl/share/licenses/$package" "usr/share/licenses/$package" "$package"; done
printf '/lib:/usr/local/lib:/usr/lib:/usr/lib/reliefos:/usr/lib/leonos\n' > "$work/data/ld.path"
file "$work/data/ld.path" etc/ld-musl-x86_64.path 0644 musl
file "$out/generated/system/kerneldebug.sys" usr/lib/reliefos/kerneldebug.sys 0755
for driver in "$out/generated/drivers"/*.drv; do file "$driver" "usr/lib/reliefos/drivers/${driver##*/}" 0755; done
file "$out/userland/motd.elf" usr/lib/reliefos/motd-status 0755 reliefos-base
file "$out/userland/dynlinkerror.elf" usr/lib/reliefos/apps/dynlinkerror/dynlinkerror.elf 0755 reliefos-apps
# awk reads TSV without collapsing empty label/extension fields. No data is
# interpreted as a command; only validated component IDs are used in paths.
awk -F '\t' -v dir="$work/manifests" -v src="$src" '
$4==1 && ($2 ~ /-app$/ || $1 ~ /^(busybox|sl)$/) {
  terminal=0; ini=src "/userland/apps/" $1 "/" $1 ".app.ini";
  while ((getline line < ini)>0) {split(line,a,"="); if(a[1]=="terminal" && a[2]~/^(1|true|yes)$/) terminal=1} close(ini);
  ini=src "/userland/" $1 "/" $1 ".app.ini";
  while ((getline line < ini)>0) {split(line,a,"="); if(a[1]=="terminal" && a[2]~/^(1|true|yes)$/) terminal=1} close(ini);
  f=dir "/" $1 ".ini";
  printf "[app]\nid=%s\nname=%s\nversion=system\ncategory=%s\nexec=%s.elf\nicon=%s\nentry=%d\nterminal=%d\nsystem=%d\nhidden=0\nopen_with=%d\nextensions=%s\ncommands=%s\n", $1,$8,$9,$1,($5?$1 ".bmp":""),$5,terminal,($2=="system-app"),$11,$10,$1 > f; close(f);
}' "$metadata"
awk -F '\t' '$4==1 && $2 ~ /-app$/ {print $1, $5}' "$metadata" > "$work/apps"
while read -r app entry; do
    case $app in sudo|su) continue ;; esac
    # M1 service role gids (kernel LEONOS_GID_*, images.sh re-chown, /etc/group
    # leonos-window-server/leonos-service): authority rides on the image gid.
    gid=0
    case $app in desktop) gid=60001 ;; windowd|imd) gid=60002 ;; esac
    file "$out/userland/$app.elf" "usr/lib/reliefos/apps/$app/$app.elf" 0755 "$app" unique "$gid"
    file "$work/manifests/$app.ini" "usr/lib/reliefos/apps/$app/manifest.ini" 0644 "$app"
    if [ "$entry" = 1 ]; then
        file "$src/resources/build-art/app-icons/$app.bmp" "usr/lib/reliefos/apps/$app/$app.bmp" 0644 "$app"
        for ini in "$src/userland/apps/$app/$app.app.ini" "$src/userland/$app/$app.app.ini"; do
            if [ -f "$ini" ]; then file "$ini" "usr/lib/reliefos/apps/$app/$app.app.ini" 0644 "$app"; break; fi
        done
    fi
    link "usr/bin/$app" "../lib/reliefos/apps/$app/$app.elf" "$app"
done < "$work/apps"
# Tool executables keep their native locations; registry entries use a local
# symlink so the registry's relative exec contract remains the same as apps.
for app in busybox sl; do
    if enabled "$app"; then
        file "$work/manifests/$app.ini" "usr/lib/reliefos/apps/$app/manifest.ini" 0644 "$app"
        target=/usr/bin/$app
        [ "$app" != busybox ] || target=/bin/busybox
        link "usr/lib/reliefos/apps/$app/$app.elf" "$target" "$app"
    fi
done
for package in ncurses; do
    if enabled "$package"; then
        # Only runtime data and commands belong in the root (development static
        # archives/headers are exported by SDK rules instead).
        for directory in bin share; do
            input=$out/upstream/$package/root/usr/$directory
            if [ -d "$input" ]; then tree "$input" "usr/$directory" "$package"; fi
        done
        tree "$out/upstream/ncurses/root/usr/share/terminfo" etc/terminfo ncurses
    fi
done
for app in fastfetch sl; do
    if enabled "$app"; then file "$out/userland/$app.elf" "usr/bin/$app" 0755 "$app" override; fi
done
for spec in 'sqlite sqlite.so.3'; do
    set -- $spec
    if enabled "$1"; then file "$out/userland/$2" "usr/lib/$2" 0755 "$1"; fi
done
if enabled portablegl || enabled glxgears; then file "$out/system/lib/libportablegl.so.1" usr/lib/libportablegl.so.1 0755 portablegl; fi
if enabled fastfetch; then
    file "$src/userland/fastfetch/config.jsonc" etc/fastfetch/config.jsonc 0644 fastfetch
    file "$src/userland/fastfetch/leonos-ascii.txt" usr/share/fastfetch/leonos-ascii.txt 0644 fastfetch
    file "$src/userland/fastfetch/hyfetch.json" etc/skel/.config/hyfetch.json 0644 fastfetch
fi
for spec in 'busybox third_party/busybox/LICENSE' 'sl third_party/sl/LICENSE' 'pleditor third_party/pl_editor/LICENSE'; do
    set -- $spec
    if enabled "$1"; then file "$src/$2" "usr/share/licenses/$1/${2##*/}" 0644 "$1" override; fi
done
for name in leonos-rpr-apkcheck leonos-rpr-ping leonos-kernel-update leonos-check-update leonos-grub-installer; do file "$src/userland/storage/$name" "usr/sbin/$name" 0755 reliefos-apps; done
for spec in 'reliefos-rpr-apkcheck leonos-rpr-apkcheck' 'reliefos-rpr-ping leonos-rpr-ping' 'reliefos-kernel-update leonos-kernel-update' 'reliefos-check-update leonos-check-update'; do
    set -- $spec; link "usr/sbin/$1" "$2" reliefos-apps
done
sh "$src/tools/build/rpr-config.sh" "$config" > "$work/data/rpr.conf"
file "$work/data/rpr.conf" etc/reliefos/rpr.conf
file "$config" etc/reliefos/leonos.conf
for source in "$src/system/config"/*; do
    [ "${source##*/}" = display.conf ] || file "$source" "etc/reliefos/${source##*/}" 0644 product-policy override
done
awk 'BEGIN{theme="metro";mode="fill"} /^CONFIG_VMDK_DEFAULT_THEME_WIN95=y$/{theme="win95"} /^CONFIG_VMDK_WALLPAPER_STRETCH=y$/{mode="stretch"} /^CONFIG_VMDK_WALLPAPER_CENTER=y$/{mode="center"} END{print "theme="theme;print "wallpaper.mode="mode}' "$config" > "$work/data/display.conf"
file "$work/data/display.conf" etc/reliefos/display.conf
awk -F '\t' 'BEGIN{print "# Generated from component selection."} $4==1 && $5==0 && $2 ~ /-app$/ {print "hide=/usr/lib/reliefos/apps/"$1"/"$1".elf"}' "$metadata" > "$work/data/desktop-entries.conf"
file "$work/data/desktop-entries.conf" etc/reliefos/desktop-entries.conf
locale_name=$(awk -F= 'BEGIN{lang="zh_CN.UTF-8"} /^CONFIG_VMDK_DEFAULT_LANG=/{gsub(/"/,"",$2);lang=$2} END{print lang}' "$config")
case $locale_name in *[!A-Za-z0-9._@-]*|'') echo 'rootfs-stage: invalid locale name' >&2; exit 2;; esac
printf 'LANG=%s\nMUSL_LOCPATH=/usr/share/musl/locales\n' "$locale_name" > "$work/data/locale.conf"
file "$work/data/locale.conf" etc/reliefos/locale.conf 0644 product-policy override
for loc in $(cat "$src/configs/nls/LINGUAS"); do
    file "$out/generated/nls/$loc/LC_MESSAGES/leonos.mo" "usr/share/locale/$loc/LC_MESSAGES/leonos.mo" 0644 reliefos-nls
    if [ -f "$out/generated/musl-locales/$loc.UTF-8" ]; then
        file "$out/generated/musl-locales/$loc.UTF-8" "usr/share/musl/locales/$loc.UTF-8" 0644 reliefos-nls
    fi
done
for source in "$src/system/docs"/*.hlp; do file "$source" "usr/share/doc/reliefos/${source##*/}"; done
for loc in $(cat "$src/configs/nls/LINGUAS"); do
    for source in "$src/system/docs/$loc"/*.hlp; do
        test -f "$source" || continue
        file "$source" "usr/share/doc/reliefos/$loc/${source##*/}"
    done
done
file "$src/logo.png" usr/share/reliefos/resources/logo.png
file "$src/system/resources/mouse.bmp" usr/share/reliefos/resources/mouse.bmp
file "$src/system/resources/wallpaper-metro.bmp" usr/share/reliefos/resources/wallpaper-metro.bmp
file "$src/system/certs/cacert.pem" etc/ssl/certs/ca-certificates.crt
file "$src/docs/APK_PREPARATION.md" usr/share/doc/reliefos/APK_PREPARATION.md
file "$src/configs/apk-ownership.json" usr/share/reliefos/apk-ownership.json
file "$src/third_party/portablegl/LICENSE" usr/share/doc/reliefos/PORTABLEGL-LICENSE
for name in leonos-metro.ttf leonos-win95.ttf; do file "$out/generated/fonts/$name" "usr/share/fonts/reliefos/$name"; done
for name in simsun.ttc system.psf; do file "$src/system/fonts/$name" "usr/share/fonts/reliefos/$name"; done
file "$src/system/fonts/times.ttf" usr/share/fonts/reliefos/times-new-roman.ttf
for source in "$src/resources/build-art/window-buttons"/*.bmp "$src/resources/build-art/minesweeper"/*.bmp; do file "$source" "usr/share/reliefos/resources/${source##*/}"; done
file "$src/test/test.mp3" test/test.mp3
for app in leonmmcoset xiaobai; do
    if enabled "$app"; then file "$src/userland/apps/$app/$app.png" "usr/lib/reliefos/apps/$app/$app.png" 0644 "$app"; fi
done
if enabled stardusthello || enabled stardustlayout || enabled stardustshowcase; then
    for name in md3-light.theme.json md3-dark.theme.json green_light.theme.json green_dark.theme.json; do
        file "$src/third_party/stardustui/docs/zh-cn/example/$name" "etc/stardustui/theme/$name" 0644 stardustui
    done
fi
# Compatibility links are explicit overrides of upstream aliases.
link usr/bin/su ../../bin/su authentication override
for command in fdisk sfdisk runuser fsck blkid mkfs.ext4 fsck.ext4 mkfs.ext2 fsck.ext2 mkfs.fat fsck.fat mkfs.exfat fsck.exfat leonos-grub-installer; do link "sbin/$command" "../usr/sbin/$command" storage override; done
link sbin/umount ../bin/umount storage override
for verb in mkfs fsck; do
    for suffix in vfat fat32; do
        link "usr/sbin/$verb.$suffix" "$verb.fat" storage override
        link "sbin/$verb.$suffix" "../usr/sbin/$verb.$suffix" storage override
    done
done
if enabled busybox; then
    file "$out/userland/busybox.elf" bin/busybox 0755 busybox
    link bin/sh busybox busybox override
    # Append only unclaimed applets; upstream commands and explicit links win.
    while IFS= read -r guest; do
        case $guest in /bin/*|/sbin/*|/usr/bin/*|/usr/sbin/*) ;; *) echo "invalid BusyBox link: $guest" >&2; exit 1 ;; esac
        case $guest in */../*|*/./*|*/busybox) echo 'unsafe BusyBox path' >&2; exit 1 ;; esac
        if awk -F '\t' -v path="$guest" '$3==path{found=1} END{exit !found}' "$plan"; then continue; fi
        # Tree rules also own actual paths; ask whether the original stage has it.
        claimed=0
        for package in libmd libbsd util-linux sudo shadow e2fsprogs dosfstools exfatprogs ncurses; do
            case $package in ncurses) enabled "$package" || continue ;; esac
            if [ -e "$out/upstream/$package/root$guest" ] || [ -L "$out/upstream/$package/root$guest" ]; then claimed=1; break; fi
        done
        [ "$claimed" = 0 ] || continue
        case $guest in /bin/*) target=busybox ;; /sbin/*) target=../bin/busybox ;; *) target=../../bin/busybox ;; esac
        link "${guest#/}" "$target" busybox
    done < "$out/userland/busybox.links"
fi
# Generated inputs retain stable, inspectable source paths in the JSON manifest.
inputs=$(dirname "$dest")/inputs
mkdir -p "$inputs"
cp -a "$work/data" "$work/manifests" "$inputs/"
awk -F '\t' -v OFS='\t' -v prefix="$work/" -v dest="$inputs/" \
    'index($2,prefix)==1 {$2=dest substr($2,length(prefix)+1)} {print}' "$plan" > "$work/published.plan"
"$stage_tool" "$work/published.plan" "$work/root" "$work/manifest.json"
find "$work/root" -exec touch -h -d "@$epoch" {} +
# Keep generated source payloads beside the manifest so its sources remain
# inspectable after temporary staging cleanup.
if [ -d "$dest.previous" ] && [ ! -e "$dest" ]; then mv "$dest.previous" "$dest"; fi
rm -rf "$dest.previous"
if [ -e "$dest" ]; then mv "$dest" "$dest.previous"; fi
mv "$work/root" "$dest"
mv "$work/manifest.json" "$manifest"
rm -rf "$dest.previous"
