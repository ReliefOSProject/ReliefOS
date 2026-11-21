#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
cd "$repo_root"
work=${TMPDIR:-/tmp}/reliefos-desktop-backend.$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work"

# The FLTK X11 frontend is the only desktop frontend; the backend choice is
# gone from the configuration.
if grep -q 'DESKTOP_BACKEND' Kconfig configs/default.conf; then
    echo 'desktop backend choice is still configurable' >&2
    exit 1
fi

make -s O="$work/default" defconfig
if grep -q 'DESKTOP_BACKEND' "$work/default/.config"; then
    echo 'defconfig still emits desktop backend symbols' >&2
    exit 1
fi

hostcc=${HOSTCC:-cc}
mkdir -p "$work/src/system" "$work/out"
cp -a system/rootfs "$work/src/system/rootfs"
# A restrictive checkout must not turn public guest policy into root-only
# files. Preserve the Git executable bit while constraining every other bit.
find "$work/src/system/rootfs" -type d -exec chmod 0700 {} +
find "$work/src/system/rootfs" -type f ! -perm /111 -exec chmod 0600 {} +
printf 'private policy fixture\n' > "$work/src/system/rootfs/etc/private-fixture"
chmod 0600 "$work/src/system/rootfs/etc/private-fixture"
for name in certs config docs fonts resources test-accounts xorg; do
    ln -s "$repo_root/system/$name" "$work/src/system/$name"
done
for name in configs Documentation resources test third_party tools userland; do
    ln -s "$repo_root/$name" "$work/src/$name"
done
cp logo.png "$work/src/logo.png"

mkdir -p "$work/out/upstream/sudo/root/usr/bin" \
    "$work/out/upstream/shadow/root/usr/bin" \
    "$work/out/pam/root/sbin" \
    "$work/out/sysroot/musl/lib" \
    "$work/out/sysroot/musl/share/licenses/musl" \
    "$work/out/sysroot/musl/share/licenses/mimalloc" \
    "$work/out/system/lib" "$work/out/generated/system" \
    "$work/out/generated/fonts" \
    "$work/out/userland"
printf '%s\n' fixture > "$work/out/upstream/sudo/root/usr/bin/sudo"
printf '%s\n' fixture > "$work/out/upstream/shadow/root/usr/bin/passwd"
printf '%s\n' fixture > "$work/out/pam/root/sbin/unix_chkpwd"
printf '%s\n' fixture > "$work/out/sysroot/musl/lib/libc.so"
printf '%s\n' fixture > "$work/out/sysroot/musl/lib/libmimalloc.so.3"
printf '%s\n' fixture > "$work/out/sysroot/musl/share/licenses/musl/LICENSE"
printf '%s\n' fixture > "$work/out/sysroot/musl/share/licenses/mimalloc/LICENSE"
printf '%s\n' fixture > "$work/out/system/lib/libreliefos.so.2"
legacy_brand=leon
legacy_brand=${legacy_brand}os
printf '%s\n' fixture > "$work/out/system/lib/lib${legacy_brand}.so.2"
# This layout-only fixture represents the separately verified kernel bundle.
# Real identity/checksum and dependency validation belongs to test-kernel-payload.
mkdir -p "$work/out/stage/kernel-payload/lib/modules/desktop-fixture" \
    "$work/out/stage/kernel-payload/usr/lib/reliefos/kernel/desktop-fixture"
printf '{"schema":1,"release":"desktop-fixture"}\n' > "$work/out/stage/kernel-payload/kernel-payload.json"
printf 'CONFIG_MODULES=y\n' > "$work/out/stage/kernel-payload/lib/modules/desktop-fixture/config"
printf 'kernel fixture\n' > "$work/out/stage/kernel-payload/usr/lib/reliefos/kernel/desktop-fixture/kernel.sys"
mkdir -p "$work/out/userland/initramfs"
printf 'kernel bundle tool fixture\n' > "$work/out/userland/initramfs/kernel-bundle.elf"
printf '%s\n' fixture > "$work/out/userland/motd.elf"
printf '%s\n' fixture > "$work/out/userland/bootinstall.elf"
printf 'native update core fixture\n' > "$work/out/userland/systemupdate.elf"
for command in sbin/grub-install sbin/grub-bios-setup sbin/grub-probe bin/grub-mkimage; do
    mkdir -p "$work/out/upstream/grub/root/usr/${command%/*}"
    printf '%s\n' fixture > "$work/out/upstream/grub/root/usr/$command"
done
mkdir -p "$work/out/upstream/grub/root/usr/lib/grub/i386-pc" \
    "$work/out/upstream/grub/root/usr/share/licenses/grub"
printf '%s\n' fixture > "$work/out/upstream/grub/root/usr/lib/grub/i386-pc/boot.img"
printf '%s\n' fixture > "$work/out/upstream/grub/root/usr/share/licenses/grub/COPYING"
for locale in $(cat configs/nls/LINGUAS); do
    mkdir -p "$work/out/generated/nls/$locale/LC_MESSAGES"
    for domain in $(cat configs/nls/DOMAINS); do
        printf '%s\n' fixture > "$work/out/generated/nls/$locale/LC_MESSAGES/$domain.mo"
    done
done
printf 'busybox\ttool\t1\t1\t0\t0\t0\tBusyBox\tTools\t\t0\n' > "$work/metadata"
for app in fileman installer leonmmcoset reliefword reliefppt reliefsettings soundctl sudo fastfetch; do
    entry=0
    case $app in fileman|reliefsettings|fastfetch) entry=1 ;; esac
    printf '%s\tsystem-app\t1\t1\t%s\t0\t0\t%s\tSystem applications\t\t0\n' "$app" "$entry" "$app" >> "$work/metadata"
    printf 'app fixture\n' > "$work/out/userland/$app.elf"
done
printf 'reliefmath\tprogram-app\t1\t1\t1\t0\t0\tReliefMath\tOffice applications\t.rmath\t1\n' >> "$work/metadata"
printf 'math GUI fixture\n' > "$work/out/userland/reliefmath.elf"
printf 'math worker fixture\n' > "$work/out/userland/reliefmath-render.elf"
printf 'reliefmathpp\tprogram-app\t1\t1\t1\t0\t0\tReliefMath++\tOffice applications\t.rmathpp\t1\n' >> "$work/metadata"
printf 'geometry GUI fixture\n' > "$work/out/userland/reliefmathpp.elf"
printf 'reliefplayer\tprogram-app\t1\t1\t1\t0\t0\tReliefPlayer\tNetwork and multimedia\t.mp3,.wav,.mp4\t1\n' >> "$work/metadata"
printf 'media player fixture\n' > "$work/out/userland/reliefplayer.elf"
mkdir -p "$work/out/upstream/ffmpeg/root/usr/lib" \
    "$work/out/upstream/ffmpeg/root/usr/include/libavcodec" \
    "$work/out/upstream/ffmpeg/root/usr/share/licenses/ffmpeg"
mkdir -p "$work/out/upstream/netsurf/root/usr/bin"
printf 'NetSurf fixture\n' > "$work/out/upstream/netsurf/root/usr/bin/netsurf"
chmod 755 "$work/out/upstream/netsurf/root/usr/bin/netsurf"
for lib in avformat avcodec avutil swresample swscale; do
    printf 'codec fixture\n' > "$work/out/upstream/ffmpeg/root/usr/lib/lib$lib-reliefplayer.so.62.1.0"
    ln -s "lib$lib-reliefplayer.so.62.1.0" "$work/out/upstream/ffmpeg/root/usr/lib/lib$lib-reliefplayer.so.62"
done
printf 'development header\n' > "$work/out/upstream/ffmpeg/root/usr/include/libavcodec/avcodec.h"
printf 'LGPL fixture\n' > "$work/out/upstream/ffmpeg/root/usr/share/licenses/ffmpeg/COPYING.LGPLv2.1"
printf 'audio fixture\n' > "$work/out/system/lib/libreliefos-audio.so.1"
printf 'calc\tprogram-app\t0\t0\t1\t0\t0\tCalculator\tOffice applications\t\t0\n' >> "$work/metadata"
printf 'ncurses\tlibrary\t1\t1\t0\t1\t0\tncurses\tLibraries\t\t0\n' >> "$work/metadata"
mkdir -p "$work/out/upstream/ncurses/root/usr/bin" \
    "$work/out/upstream/ncurses/root/usr/share/terminfo" \
    "$work/out/upstream/ncurses/root/usr/include" \
    "$work/out/upstream/fixture-provider/root/usr/bin" \
    "$work/out/upstream/fixture-provider/root/sbin" \
    "$work/out/upstream/inactive/root/usr/bin"
printf 'ncurses clear\n' > "$work/out/upstream/ncurses/root/usr/bin/clear"
ln -s clear "$work/out/upstream/ncurses/root/usr/bin/reset"
printf 'terminal data\n' > "$work/out/upstream/ncurses/root/usr/share/terminfo/fixture"
printf 'development header\n' > "$work/out/upstream/ncurses/root/usr/include/curses.h"
printf 'selected provider\n' > "$work/out/upstream/fixture-provider/root/usr/bin/hexdump"
printf 'external ping provider\n' > "$work/out/upstream/fixture-provider/root/usr/bin/ping"
printf 'storage provider\n' > "$work/out/upstream/fixture-provider/root/sbin/mkfs.ext4"
printf 'inactive provider\n' > "$work/out/upstream/inactive/root/usr/bin/tree"
printf 'BusyBox fixture\n' > "$work/out/userland/busybox.elf"
printf '%s\n' /bin/sh /bin/ash /bin/false /bin/ping /bin/ping6 /bin/hexdump \
    /usr/bin/clear /usr/bin/reset /sbin/mkfs.ext4 /bin/passwd /usr/bin/tree > "$work/out/userland/busybox.links"

$hostcc -std=c11 -Wall -Wextra -Werror -Wpedantic -I"$repo_root" \
    "$repo_root/tools/host/manifest/reliefos-stage.c" \
    "$repo_root/tools/host/common/io.c" "$repo_root/tools/host/common/buffer.c" \
    "$repo_root/tools/host/manifest/json.c" -o "$work/stage"
$hostcc -std=c11 -Wall -Wextra -Werror -Wpedantic -I"$repo_root" \
    "$repo_root/tools/host/manifest/reliefos-layout.c" -o "$work/layout"

cat > "$work/rootfs.config" <<CONFIG
CONFIG_RPR_BASE_URL="https://example.invalid/rpr"
CONFIG_VMDK_DEFAULT_LANG="zh_CN.UTF-8"
CONFIG
(umask 077
ROOTFS_UPSTREAM_PACKAGES='sudo shadow fixture-provider ffmpeg netsurf' \
sh "$repo_root/tools/build/rootfs-stage.sh" "$work/src" "$work/out" \
    "$work/rootfs.config" "$work/metadata" "$work/stage" "$work/layout" \
    "$work/root" "$work/manifest.json" 1700000000)
xorg_root=$work/root
# Verify the resulting policy rather than the host fixture owner's ability to
# read files. This also checks public ancestors and unchanged sensitive modes.
python3 - "$xorg_root" "$work/manifest.json" <<'PY'
import json
from pathlib import Path
import stat
import sys
root = Path(sys.argv[1])
entries = {entry['path']: entry for entry in json.loads(Path(sys.argv[2]).read_text())['entries']}
public = ('etc/profile', 'etc/profile.d/reliefos-locale.sh', 'etc/bash/reliefos.sh',
          'etc/bash/colors.sh', 'etc/skel/.bashrc', 'etc/skel/.bash_profile',
          'etc/skel/.inputrc', 'etc/passwd', 'etc/group', 'etc/hosts',
          'etc/nsswitch.conf', 'etc/services', 'etc/protocols', 'etc/os-release',
          'etc/motd', 'etc/pam.d/login', 'etc/security/limits.conf',
          'etc/gtk-3.0/settings.ini', 'etc/reliefos/icewm/preferences',
          'etc/reliefos/icewm/themes/light/default.theme',
          'usr/share/licenses/netbase/copyright')
for relative in public:
    path = root / relative
    assert stat.S_IMODE(path.stat().st_mode) == 0o644, relative
    assert entries['/' + relative]['mode'] == '0644', relative
    parent = path.parent
    while parent != root:
        assert stat.S_IMODE(parent.stat().st_mode) == 0o755, parent
        parent = parent.parent
for relative in ('etc/init.d/reliefos-dhcp', 'usr/lib/reliefos/console-session',
                 'etc/reliefos/icewm/terminal.sh'):
    assert stat.S_IMODE((root / relative).stat().st_mode) == 0o755, relative
for relative, mode in (('etc/shadow', 0o600), ('etc/gshadow', 0o600),
                       ('etc/sudoers', 0o440), ('etc/private-fixture', 0o600),
                       ('root', 0o700), ('etc/sudoers.d', 0o750)):
    assert stat.S_IMODE((root / relative).stat().st_mode) == mode, relative
print('PASS public shell/desktop policy under umask0077 and source0600; sensitive policy preserved')
PY
# The legacy nine-argument caller still selects the default payload directory.
cmp "$work/out/stage/kernel-payload/lib/modules/desktop-fixture/config" \
    "$xorg_root/lib/modules/desktop-fixture/config"
cmp "$work/out/stage/kernel-payload/usr/lib/reliefos/kernel/desktop-fixture/kernel.sys" \
    "$xorg_root/usr/lib/reliefos/kernel/desktop-fixture/kernel.sys"

# Exercise the real rootfs recipe and Kbuild parameter invalidation in this
# disposable host stage. An older alternate payload must replace the default
# one when O_STAGE changes, and an unchanged selection must remain a no-op.
alternate_stage=$work/alternate-stage
mkdir -p "$alternate_stage/kernel-payload/lib/modules/alternate-fixture" \
    "$alternate_stage/kernel-payload/usr/lib/reliefos/kernel/alternate-fixture"
printf '{"schema":1,"release":"alternate-fixture"}\n' > "$alternate_stage/kernel-payload/kernel-payload.json"
printf 'alternate module fixture\n' > "$alternate_stage/kernel-payload/lib/modules/alternate-fixture/config"
printf 'alternate kernel fixture\n' > "$alternate_stage/kernel-payload/usr/lib/reliefos/kernel/alternate-fixture/kernel.sys"
# Both manifests predate every raw stage, so dependency timestamps alone cannot
# hide a missing selected-path parameter in either direction.
touch -d @1 "$work/out/stage/kernel-payload/kernel-payload.json" \
    "$alternate_stage/kernel-payload/kernel-payload.json"
mkdir -p "$work/host-bin" "$work/out/meta"
ln -s "$work/stage" "$work/host-bin/reliefos-stage"
ln -s "$work/layout" "$work/host-bin/reliefos-layout"
for package in sudo shadow fixture-provider ffmpeg netsurf grub; do
    touch "$work/out/upstream/$package/root/.complete"
done
cat > "$work/rootfs.mk" <<'MAKE'
include $(FIXTURE_REPO)/scripts/Kbuild.include
include $(FIXTURE_REPO)/scripts/reliefos/Makefile.parameters
RELIEFOS_SRC := $(FIXTURE_SRC)
O := $(FIXTURE_OUT)
O_META := $(O)/meta
RELIEFOS_HOST_BIN := $(FIXTURE_HOST_BIN)
RELIEFOS_CONFIG_FILE := $(FIXTURE_CONFIG)
COMPONENT_METADATA := $(FIXTURE_METADATA)
USERLAND_DIR := $(O)/userland
UPSTREAM_ROOT := $(O)/upstream
UPSTREAM_PACKAGES := sudo shadow fixture-provider ffmpeg netsurf
GRUB_GUEST_ROOT := $(UPSTREAM_ROOT)/grub/root
RELIEFOS_KERNEL_PAYLOAD := $(O_STAGE)/kernel-payload
RELIEFOS_KERNEL_PAYLOAD_MANIFEST := $(RELIEFOS_KERNEL_PAYLOAD)/kernel-payload.json
SOURCE_DATE_EPOCH := 1700000000
Q := @
quiet := quiet_
PHONY := FORCE
.PHONY: FORCE
FORCE:
include $(FIXTURE_REPO)/scripts/reliefos/Makefile.rootfs
# Limit source hashing to a stable fixture input; the real recipe, dependencies
# and saved-command comparison remain unchanged.
ROOTFS_SOURCE_DIRS := $(RELIEFOS_CONFIG_FILE)
-include $(wildcard $(O_META)/.*.cmd)
MAKE
stage_rootfs() (
    unset MAKEFLAGS MFLAGS MAKELEVEL MAKEOVERRIDES
    make --no-print-directory -rR -f "$work/rootfs.mk" \
        "FIXTURE_REPO=$repo_root" "FIXTURE_SRC=$work/src" \
        "FIXTURE_OUT=$work/out" "FIXTURE_HOST_BIN=$work/host-bin" \
        "FIXTURE_CONFIG=$work/rootfs.config" "FIXTURE_METADATA=$work/metadata" \
        "O_STAGE=$1" rootfs-raw
)
stage_rootfs "$work/out/stage"
raw_stamp=$(stat -c %y "$work/out/rootfs/raw.complete")
parameter_stamp=$(stat -c %y "$work/out/meta/rootfs.parameters")
stage_rootfs "$work/out/stage"
test "$raw_stamp" = "$(stat -c %y "$work/out/rootfs/raw.complete")"
test "$parameter_stamp" = "$(stat -c %y "$work/out/meta/rootfs.parameters")"
stage_rootfs "$alternate_stage"
cmp "$alternate_stage/kernel-payload/lib/modules/alternate-fixture/config" \
    "$work/out/rootfs/raw/lib/modules/alternate-fixture/config"
cmp "$alternate_stage/kernel-payload/usr/lib/reliefos/kernel/alternate-fixture/kernel.sys" \
    "$work/out/rootfs/raw/usr/lib/reliefos/kernel/alternate-fixture/kernel.sys"
test ! -e "$work/out/rootfs/raw/lib/modules/desktop-fixture"
test ! -e "$work/out/rootfs/raw/usr/lib/reliefos/kernel/desktop-fixture"
grep -Fq "kernel-payload=$alternate_stage/kernel-payload" "$work/out/meta/rootfs.parameters"
stage_rootfs "$work/out/stage"
test -f "$work/out/rootfs/raw/lib/modules/desktop-fixture/config"
test ! -e "$work/out/rootfs/raw/lib/modules/alternate-fixture"
# An explicit missing payload must fail instead of using the existing default.
if ROOTFS_UPSTREAM_PACKAGES='sudo shadow fixture-provider ffmpeg netsurf' \
    sh "$repo_root/tools/build/rootfs-stage.sh" "$work/src" "$work/out" \
    "$work/rootfs.config" "$work/metadata" "$work/stage" "$work/layout" \
    "$work/root-missing-payload" "$work/missing-manifest.json" 1700000000 \
    "$work/missing-kernel-payload" > "$work/missing-payload.log" 2>&1; then
    echo 'FAIL - explicit missing kernel payload silently used default' >&2
    exit 1
fi
grep -Fq "matching kernel payload is missing: $work/missing-kernel-payload" "$work/missing-payload.log"
test ! -e "$work/root-missing-payload"
test ! -e "$work/missing-manifest.json"
printf '%s\n' 'ok - selected kernel payload, legacy default and O_STAGE invalidation'

cmp "$work/out/userland/systemupdate.elf" "$xorg_root/usr/sbin/reliefos-update"
test -x "$xorg_root/usr/sbin/reliefos-update"
test "$(readlink "$xorg_root/lib/apk/commit_hooks.d/reliefos-update")" = /usr/sbin/reliefos-update
for prefix in reliefos leonos; do
    for command in kernel-update check-update rpr-apkcheck rpr-ping; do
        test "$(readlink "$xorg_root/usr/sbin/$prefix-$command")" = reliefos-update
    done
done
for locale in $(cat configs/nls/LINGUAS); do
    for domain in $(cat configs/nls/DOMAINS); do
        cmp "$work/out/generated/nls/$locale/LC_MESSAGES/$domain.mo" \
            "$xorg_root/usr/share/locale/$locale/LC_MESSAGES/$domain.mo"
    done
done
for lib in avformat avcodec avutil swresample swscale; do
    test -f "$xorg_root/usr/lib/lib$lib-reliefplayer.so.62"
    test -L "$xorg_root/usr/lib/lib$lib-reliefplayer.so.62"
done
test -f "$xorg_root/usr/share/licenses/ffmpeg/COPYING.LGPLv2.1"
test ! -e "$xorg_root/usr/include/libavcodec"
grep -qx 'extensions=.mp3,.wav,.mp4' "$xorg_root/usr/lib/reliefos/apps/reliefplayer/manifest.ini"
awk -F '\t' '$1!="reliefplayer"' "$work/metadata" > "$work/metadata-no-player"
ROOTFS_UPSTREAM_PACKAGES='sudo shadow fixture-provider ffmpeg netsurf' \
sh "$repo_root/tools/build/rootfs-stage.sh" "$work/src" "$work/out" \
    "$work/rootfs.config" "$work/metadata-no-player" "$work/stage" "$work/layout" \
    "$work/root-no-player" "$work/no-player-manifest.json" 1700000000
test ! -e "$work/root-no-player/usr/bin/reliefplayer"
test ! -e "$work/root-no-player/usr/lib/libavformat-reliefplayer.so.62"
test ! -e "$work/root-no-player/usr/share/licenses/ffmpeg"

# Only self-authored graphical apps receive artwork, including hidden entries.
for app in fileman installer leonmmcoset reliefword reliefppt reliefmath reliefmathpp reliefsettings; do
    for size in 16 32 48 64 128 256; do
        icon=usr/share/icons/hicolor/${size}x${size}/apps/reliefos-$app.png
        cmp "$repo_root/resources/build-art/app-icons/hicolor/${size}x${size}/apps/reliefos-$app.png" "$xorg_root/$icon"
    done
done
for app in soundctl sudo fastfetch; do
    test ! -e "$xorg_root/usr/lib/reliefos/apps/$app/$app.bmp"
    for size in 16 32 48 64 128 256; do
        test ! -e "$xorg_root/usr/share/icons/hicolor/${size}x${size}/apps/reliefos-$app.png"
    done
done
grep -qx 'icon=' "$xorg_root/usr/lib/reliefos/apps/fastfetch/manifest.ini"
test ! -e "$xorg_root/usr/share/icons/hicolor/48x48/apps/reliefos-calc.png"
test -f "$xorg_root/usr/lib/reliefos/apps/installer/installer.bmp"
test -f "$xorg_root/usr/lib/reliefos/apps/leonmmcoset/leonmmcoset.bmp"
cmp "$work/out/userland/reliefmath.elf" "$xorg_root/usr/lib/reliefos/apps/reliefmath/reliefmath.elf"
cmp "$work/out/userland/reliefmath-render.elf" "$xorg_root/usr/lib/reliefos/apps/reliefmath/reliefmath-render.elf"
test "$(readlink "$xorg_root/usr/bin/reliefmath")" = ../lib/reliefos/apps/reliefmath/reliefmath.elf
grep -qx 'open_with=1' "$xorg_root/usr/lib/reliefos/apps/reliefmath/manifest.ini"
grep -qx 'extensions=.rmath' "$xorg_root/usr/lib/reliefos/apps/reliefmath/manifest.ini"
grep -qx 'category=Office applications' "$xorg_root/usr/lib/reliefos/apps/reliefmath/manifest.ini"
test "$(stat -c %a "$xorg_root/usr/share/reliefmath/STIXTwoMath-Regular.otf")" = 444
test "$(stat -c %a "$xorg_root/usr/share/licenses/reliefmath/STIX-OFL.txt")" = 444
test -f "$xorg_root/usr/share/licenses/reliefmath/README.md"
test -f "$xorg_root/usr/share/mime/packages/reliefmath.xml"
cmp "$work/out/userland/reliefmathpp.elf" "$xorg_root/usr/lib/reliefos/apps/reliefmathpp/reliefmathpp.elf"
test -f "$xorg_root/usr/share/mime/packages/reliefmathpp.xml"
grep -qx 'extensions=.rmathpp' "$xorg_root/usr/lib/reliefos/apps/reliefmathpp/manifest.ini"
awk -F '\t' '$1!="reliefmathpp"' "$work/metadata" > "$work/metadata-no-mathpp"
sh "$repo_root/tools/build/rootfs-stage.sh" "$work/src" "$work/out" \
    "$work/rootfs.config" "$work/metadata-no-mathpp" "$work/stage" "$work/layout" \
    "$work/root-no-mathpp" "$work/no-mathpp-manifest.json" 1700000000
for path in usr/lib/reliefos/apps/reliefmathpp usr/bin/reliefmathpp usr/share/applications/reliefos-reliefmathpp.desktop usr/share/mime/packages/reliefmathpp.xml; do
    test ! -e "$work/root-no-mathpp/$path"
done

# Removing the selected app from component metadata excludes every private
# product while leaving the shared system CJK font in the composed root.
awk -F '\t' '$1!="reliefmath"' "$work/metadata" > "$work/metadata-no-reliefmath"
ROOTFS_UPSTREAM_PACKAGES='sudo shadow fixture-provider' \
sh "$repo_root/tools/build/rootfs-stage.sh" "$work/src" "$work/out" \
    "$work/rootfs.config" "$work/metadata-no-reliefmath" "$work/stage" "$work/layout" \
    "$work/root-no-reliefmath" "$work/no-reliefmath-manifest.json" 1700000000
for path in \
    usr/lib/reliefos/apps/reliefmath \
    usr/bin/reliefmath \
    usr/share/applications/reliefos-reliefmath.desktop \
    usr/share/reliefmath \
    usr/share/licenses/reliefmath \
    usr/share/mime/packages/reliefmath.xml; do
    test ! -e "$work/root-no-reliefmath/$path"
done
for size in 16 32 48 64 128 256; do
    test ! -e "$work/root-no-reliefmath/usr/share/icons/hicolor/${size}x${size}/apps/reliefos-reliefmath.png"
done
test -f "$work/root-no-reliefmath/usr/share/fonts/reliefos/simsun.ttc"

grep -qx xorg "$xorg_root/etc/reliefos/desktop-backend"
test ! -e "$xorg_root/etc/runlevels/default/reliefos-windowd"
test ! -e "$xorg_root/etc/runlevels/default/reliefos-session"
test -f "$xorg_root/etc/X11/xorg.conf"
# Mouse0 must consume the kernel's authoritative absolute position.
# xf86-input-evdev drops the absolute axes of a mixed rel/abs mouse
# ("ignoring absolute axes") and rebuilds the cursor from the relative deltas,
# which permanently drifts away from the kernel position whenever a motion
# event is lost (the VMware mouse offset bug).  IgnoreRelativeAxes "true" is
# not enough: the scrollwheel probe branch re-enables EVDEV_RELATIVE_EVENTS and
# the relative path wins.  The trinary value IgnoreAbsoluteAxes "false" means
# "unignore": both axis classes initialize and the absolute position wins every
# sync frame.
mouse0=$(awk '/Identifier "Mouse0"/,/^EndSection/' "$xorg_root/etc/X11/xorg.conf")
if ! printf '%s\n' "$mouse0" | grep -q 'Option "IgnoreAbsoluteAxes" "false"'; then
    echo 'Mouse0 must unignore the absolute axes (IgnoreAbsoluteAxes "false")' >&2
    echo 'FAIL - xorg.conf mouse absolute position contract' >&2
    exit 1
fi
test -f "$xorg_root/etc/X11/xdm/xdm-config"
test -f "$xorg_root/etc/X11/xdm/Xservers"
test -f "$xorg_root/etc/X11/xdm/Xresources"
test -x "$xorg_root/usr/lib/X11/xdm/Xsession"
test -x "$xorg_root/usr/lib/X11/xdm/Xsetup_0"
test -x "$xorg_root/usr/bin/netsurf"
test ! -e "$xorg_root/usr/bin/dillo"
test ! -e "$xorg_root/etc/reliefos/wallpaper.png"
test -x "$xorg_root/usr/lib/reliefos/reliefos-xdm"
test -x "$xorg_root/usr/lib/reliefos/xorg-tty-wrapper"
for path in etc/reliefos/xdm.conf etc/reliefos/xdm-Xservers \
    etc/reliefos/xdm-Xresources etc/reliefos/xdm-session \
    etc/reliefos/xdm-setup usr/lib/reliefos/xdm-session \
    usr/lib/reliefos/xdm-setup; do
    test ! -e "$xorg_root/$path"
done
test -f "$xorg_root/etc/pam.d/xdm"
# IceWM is the only window manager: its configuration is staged and no old
# window manager file may survive in the product root.
test -f "$xorg_root/etc/reliefos/icewm/preferences"
test -f "$xorg_root/etc/reliefos/icewm/menu"
test -f "$xorg_root/etc/reliefos/icewm/keys"
test -f "$xorg_root/etc/reliefos/icewm/themes/light/default.theme"
test ! -e "$xorg_root/etc/reliefos/twmrc"
test ! -e "$xorg_root/usr/bin/twm"
test ! -e "$xorg_root/usr/bin/twm.real"
test -f "$xorg_root/bin/busybox"
test -L "$xorg_root/bin/sh"
test -L "$xorg_root/bin/ash"
test -L "$xorg_root/bin/false"
test -L "$xorg_root/bin/ping6"
test "$(readlink "$xorg_root/bin/ping6")" = busybox
# Commands claimed by a staged upstream root keep their provider and never
# grow a BusyBox link, even under a different directory.
cmp "$work/out/upstream/fixture-provider/root/usr/bin/hexdump" "$xorg_root/usr/bin/hexdump"
cmp "$work/out/upstream/fixture-provider/root/usr/bin/ping" "$xorg_root/usr/bin/ping"
cmp "$work/out/upstream/fixture-provider/root/sbin/mkfs.ext4" "$xorg_root/sbin/mkfs.ext4"
test ! -e "$xorg_root/bin/hexdump"
test ! -e "$xorg_root/bin/ping"
# /sbin is the official provider; old /usr/sbin callers use a one-way alias.
# BusyBox must not claim either entry point.
test ! -L "$xorg_root/sbin/mkfs.ext4"
test "$(readlink "$xorg_root/usr/sbin/mkfs.ext4")" = ../../sbin/mkfs.ext4
# ncurses payloads are installed later from signed APKs, never overlaid.
grep -Fx ncurses "$xorg_root/etc/reliefos/apk-components"
test ! -e "$xorg_root/usr/share/doc/reliefos/apk-transitions"
test ! -e "$xorg_root/usr/lib/reliefos/apps/sl"
test -f "$xorg_root/usr/share/licenses/ncurses/LICENSE"
test ! -e "$xorg_root/usr/share/terminfo/fixture"
test ! -e "$xorg_root/etc/terminfo/fixture"
test ! -e "$xorg_root/usr/include/curses.h"
cmp "$work/out/upstream/shadow/root/usr/bin/passwd" "$xorg_root/usr/bin/passwd"
test ! -e "$xorg_root/bin/passwd"
# Roots outside ROOTFS_UPSTREAM_PACKAGES never reach the rootfs: the command
# stays a generated BusyBox link instead of the inactive provider file.
test -L "$xorg_root/usr/bin/tree"
test "$(readlink "$xorg_root/usr/bin/tree")" = ../../bin/busybox

printf '%s\n' 'ok - X11 session policy and upstream staging'
