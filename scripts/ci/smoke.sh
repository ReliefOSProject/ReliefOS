#!/usr/bin/env bash
set -euo pipefail

case "${SMOKE_FIRMWARE:-}" in
  bios|uefi) ;;
  *) echo 'SMOKE_FIRMWARE must be bios or uefi' >&2; exit 2 ;;
esac

root=out/x86_64/release
python3 tools/ci.py verify --root "$root"
export RELIEFOS_QMP_DIR="$RUNNER_TEMP"
export DUAL_BOOT_ACCEL=tcg
export DUAL_BOOT_CPU=max
if [[ -r /dev/kvm && -w /dev/kvm ]]; then
  export DUAL_BOOT_ACCEL=kvm
  export DUAL_BOOT_CPU=host
fi
if [[ "$SMOKE_FIRMWARE" == uefi ]]; then
  # Ubuntu's combined firmware includes both code and an initialized var store.
  export QEMU_FIRMWARE=/usr/share/ovmf/OVMF.fd
  test -s "$QEMU_FIRMWARE"
fi
printf 'Booting %s with %s acceleration\n' "$SMOKE_FIRMWARE" "$DUAL_BOOT_ACCEL"
python3 tools/test_dual_boot_qemu.py \
  --images "$root/images" --kernel-manifest "$root/kernel-install/install.bundle.json" \
  --out "out/ci-smoke/$SMOKE_FIRMWARE" --matrix boot \
  --firmwares "$SMOKE_FIRMWARE" --timeout 300
# The acceptance runner uses private disks; also recheck every input afterward.
python3 tools/ci.py verify --root "$root"
