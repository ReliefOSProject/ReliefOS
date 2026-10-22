#!/bin/sh
# Capture native run argv without launching a VM or requiring real media.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(mktemp -d)
trap 'rm -rf "$w"' EXIT HUP INT TERM
cat > "$w/Kbuild" <<EOF
include $src/scripts/reliefos/Makefile.run
.PHONY: image-vmdk iso installer
image-vmdk iso installer:
EOF
cat > "$w/qemu" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" > "$QEMU_CAPTURE"
EOF
chmod +x "$w/qemu"
touch "$w/ovmf.fd"
for goal in run run-iso run-installer; do
    for mode in bios uefi; do
        firmware=$w/missing-ovmf.fd
        [ "$mode" != uefi ] || firmware=$w/ovmf.fd
        QEMU_CAPTURE=$w/argv make --no-print-directory -f "$w/Kbuild" "$goal" \
            QEMU="$w/qemu" QEMU_BOOT="$mode" QEMU_FIRMWARE="$firmware" \
            DISK_VMDK=private.vmdk LIVE_ISO=live.iso INSTALLER_ISO=installer.iso > "$w/run.log" 2>&1 || {
            cat "$w/run.log" >&2; exit 1;
        }
        if [ "$mode" = bios ]; then
            if grep -Fxq -- -bios "$w/argv"; then echo 'FAIL - BIOS still supplies OVMF' >&2; exit 1; fi
        else
            grep -Fxq -- -bios "$w/argv"
            grep -Fxq -- "$w/ovmf.fd" "$w/argv"
        fi
    done
done
for mode in uefi unknown; do
    rm -f "$w/argv"
    if QEMU_CAPTURE=$w/argv make --no-print-directory -f "$w/Kbuild" run \
        QEMU="$w/qemu" QEMU_BOOT="$mode" QEMU_FIRMWARE="$w/missing" > "$w/fail.log" 2>&1; then
        echo 'FAIL - invalid firmware policy succeeded' >&2; exit 1
    fi
    [ ! -e "$w/argv" ]
done
printf 'PASS QEMU firmware: BIOS has no OVMF dependency, UEFI requires firmware, all3 run targets and unknown-mode rejection\n'
