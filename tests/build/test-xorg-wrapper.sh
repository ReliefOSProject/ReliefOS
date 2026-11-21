#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Exercise the real wrapper using only private fixture paths.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(mktemp -d)
trap 'rm -rf "$w"' EXIT HUP INT TERM
mkdir -p "$w/dev" "$w/sys/class/drm/card0/device" "$w/etc/X11" "$w/usr/bin"
touch "$w/dev/tty1"
cp "$src/system/xorg/xorg.conf" "$w/etc/X11/xorg.conf"
# Rebase absolute runtime paths without adding test-only environment inputs to
# the production launcher. Shell/system commands still execute normally.
sed -e "s@/dev/@$w/dev/@g" -e "s@/sys/@$w/sys/@g" \
    -e "s@/etc/@$w/etc/@g" -e "s@/run/@$w/run/@g" \
    -e "s@/usr/@$w/usr/@g" "$src/system/xorg/xorg-tty-wrapper" > "$w/wrapper"
cat > "$w/usr/bin/Xorg" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" > "$XORG_CAPTURE"
EOF
chmod +x "$w/usr/bin/Xorg"
export XORG_CAPTURE=$w/argv
printf '0x1af4\n' > "$w/sys/class/drm/card0/device/vendor"
printf '0x1050\n' > "$w/sys/class/drm/card0/device/device"
sh "$w/wrapper" :0 vt1
grep -Fxq -- "$w/run/X11/xorg-virtio.conf" "$w/argv"
grep -Fq 'Option "SWcursor" "false"' "$w/run/X11/xorg-virtio.conf"
grep -Fq 'Option "PageFlip" "true"' "$w/run/X11/xorg-virtio.conf"
cmp "$src/system/xorg/xorg.conf" "$w/etc/X11/xorg.conf"
sh "$w/wrapper" :0 -config custom.conf
test "$(cat "$w/argv")" = "$(printf ':0\n-config\ncustom.conf')"
sh "$w/wrapper" :0 -configdir custom.d
test "$(cat "$w/argv")" = "$(printf ':0\n-configdir\ncustom.d')"
printf '0x15ad\n' > "$w/sys/class/drm/card0/device/vendor"
printf '0x0405\n' > "$w/sys/class/drm/card0/device/device"
sh "$w/wrapper" :0 vt1
test "$(cat "$w/argv")" = "$(printf ':0\nvt1')"
rm "$w/sys/class/drm/card0/device/vendor"
sh "$w/wrapper" :0 vt1
test "$(cat "$w/argv")" = "$(printf ':0\nvt1')"
printf 'PASS Xorg wrapper: VirtIO cursor/flip, fbdev defaults, administrator overrides\n'
