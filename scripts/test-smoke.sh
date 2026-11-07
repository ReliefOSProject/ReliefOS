#!/bin/sh
# The acceptance runner owns one VM at a time and requires fresh kernel evidence.
set -eu
[ "$#" = 3 ] || exit 2
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
export QEMU_FIRMWARE=$3
exec python3 "$src/tools/test_dual_boot_qemu.py" --images "$1" \
    --out "${SMOKE_OUTPUT:-$src/out/dbq}" --matrix boot \
    --firmwares bios,uefi --timeout "${SMOKE_TIMEOUT:-180}"
