#!/bin/sh
# Actual signed APK and native-bundle preflight in disposable namespaces.
# Firmware activation/recovery/reboot is a separate ReliefNT guest matrix.
set -eu
[ "$#" -le 1 ] || { echo 'usage: test-kernel-update.sh [OUTPUT_DIR]' >&2; exit 2; }
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd -P)
if [ "$#" = 0 ]; then
    exec python3 "$src/tools/test_kernel_update.py"
fi
mkdir -p "$1"
output=$(CDPATH= cd -- "$1" && pwd -P)
status=0
python3 "$src/tools/test_kernel_update.py" > "$output/kernel-update.log" 2>&1 || status=$?
cat "$output/kernel-update.log"
exit "$status"
