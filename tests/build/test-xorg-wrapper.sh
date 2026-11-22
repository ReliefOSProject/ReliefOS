#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Exercise the real wrapper using only private fixture paths.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(mktemp -d)
scenario=setup
cleanup() {
    status=$?
    if [ "$status" -ne 0 ]; then
        printf 'FAIL Xorg wrapper: %s\n' "$scenario" >&2
        for output in tty1 argv fbset-argv expected; do
            path=$w/$output
            [ "$output" != tty1 ] || path=$w/dev/tty1
            if [ -f "$path" ]; then
                printf '%s:\n' "$output" >&2
                cat "$path" >&2
            fi
        done
    fi
    rm -rf "$w"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
fail() { printf '%s\n' "$1" >&2; exit 1; }
mkdir -p "$w/dev/dri" "$w/sys/class/drm/card0/device" "$w/etc/X11" "$w/usr/bin" "$w/usr/sbin"
touch "$w/dev/tty1"
# The wrapper only probes the node type. A private alias to /dev/null gives
# an unprivileged test a real character device without using host DRM/mknod.
ln -s /dev/null "$w/dev/dri/card0"
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
cat > "$w/usr/sbin/fbset" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" > "$FBSET_CAPTURE"
exit "${FBSET_STATUS:-0}"
EOF
chmod +x "$w/usr/bin/Xorg" "$w/usr/sbin/fbset"
export XORG_CAPTURE=$w/argv
export FBSET_CAPTURE=$w/fbset-argv
pci=$w/sys/class/drm/card0/device
set_pci() {
    printf '%s\n' "$1" > "$pci/vendor"
    printf '%s\n' "$2" > "$pci/device"
}
run_wrapper() {
    scenario=$1
    shift
    rm -f "$w/argv" "$w/fbset-argv" "$w/expected"
    rm -rf "$w/run/X11"
    sh "$w/wrapper" "$@" || fail 'wrapper exited unsuccessfully'
    cmp "$src/system/xorg/xorg.conf" "$w/etc/X11/xorg.conf" || fail 'system configuration changed'
}
expect_argv() {
    printf '%s\n' "$@" > "$w/expected"
    cmp "$w/expected" "$w/argv" || fail 'Xorg arguments differ'
}
expect_defaults() {
    expect_argv :0 vt1
    [ ! -d "$w/run/X11" ] || fail 'fallback generated a hardware configuration'
}

for backend in virtio svga; do
    case "$backend" in
        virtio) set_pci 0x1af4 0x1050; flip=true ;;
        svga) set_pci 0x15ad 0x0405; flip=false ;;
    esac
    run_wrapper "$backend hardware cursor configuration" :0 vt1
    config=$w/run/X11/xorg-$backend.conf
    expect_argv -config "$config" :0 vt1
    grep -Fq 'Option "SWcursor" "false"' "$config" || fail 'hardware cursor was not enabled'
    grep -Fq "Option \"PageFlip\" \"$flip\"" "$config" || fail 'unexpected page flip policy'
    [ ! -f "$w/fbset-argv" ] || fail 'DRM backend invoked fbset'
    for option in -config -configdir; do
        run_wrapper "$backend administrator $option override" :0 "$option" 'custom config path' vt1
        expect_argv :0 "$option" 'custom config path' vt1
        [ ! -d "$w/run/X11" ] || fail 'administrator override generated a hardware configuration'
    done
done

set_pci 0x1af4 0xffff
run_wrapper 'unknown PCI device' :0 vt1
expect_defaults
set_pci 0xffff 0x1050
run_wrapper 'unknown PCI vendor' :0 vt1
expect_defaults
for field in vendor device; do
    set_pci 0x1af4 0x1050
    rm "$pci/$field"
    run_wrapper "missing PCI $field" :0 vt1
    expect_defaults
done
set_pci 0x1af4 0x1050
rm "$w/dev/dri/card0"
touch "$w/dev/dri/card0"
run_wrapper 'regular file is not a DRM node' :0 vt1
expect_defaults
rm "$w/dev/dri/card0"
run_wrapper 'missing DRM node' :0 vt1
expect_defaults
[ ! -f "$w/fbset-argv" ] || fail 'missing framebuffer invoked fbset'
ln -s /dev/null "$w/dev/fb0"
ln -s /dev/null "$w/dev/dri/card0"
run_wrapper 'DRM takes precedence over framebuffer' :0 vt1
expect_argv -config "$w/run/X11/xorg-virtio.conf" :0 vt1
[ ! -f "$w/fbset-argv" ] || fail 'DRM backend invoked fbset with a framebuffer present'
rm "$w/dev/dri/card0"
for result in 0 1; do
    export FBSET_STATUS=$result
    run_wrapper "fbdev fallback with fbset exit $result" :0 vt1
    expect_defaults
    printf '%s\n' -fb "$w/dev/fb0" -depth 32 > "$w/expected"
    cmp "$w/expected" "$w/fbset-argv" || fail 'fbdev depth setup arguments differ'
done
printf 'PASS Xorg wrapper: VirtIO/SVGA cursor policy, DRM/PCI fallbacks, fbdev depth, administrator overrides\n'
