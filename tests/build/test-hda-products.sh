#!/bin/sh
# The six device drivers are linked into kernel.sys itself: the build tree must
# publish exactly the four kernel products and stage no driver modules.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
audio_out=${AUDIO_OUT:-"$repo/out/audio-hda"}
audio_root="$audio_out/rootfs/raw"
for name in kernel.sys kernel.debug; do
    for product in "$audio_out/generated/system/$name" "$audio_out/kernel-install/$name"; do
        if [ ! -s "$product" ]; then
            echo "FAIL missing product: $product" >&2
            exit 1
        fi
    done
    cmp "$audio_out/generated/system/$name" "$audio_out/kernel-install/$name"
done
for product in "$audio_out/generated/system/kerneldebug.sys" "$audio_out/kernel-install/kerneldebug.sys" \
               "$audio_root/usr/lib/reliefos/kerneldebug.sys" \
               "$audio_out/generated/boot/loader.elf" "$audio_out/kernel-install/loader.elf"; do
    if [ ! -s "$product" ]; then
        echo "FAIL missing product: $product" >&2
        exit 1
    fi
done
if find "$audio_out/generated" "$audio_out/kernel-install" "$audio_root/usr/lib" -name '*.drv' | grep -q .; then
    echo "FAIL stale .drv driver modules are still produced or staged" >&2
    exit 1
fi
if [ -d "$audio_root/usr/lib/reliefos/drivers" ]; then
    echo "FAIL stale staged driver directory: $audio_root/usr/lib/reliefos/drivers" >&2
    exit 1
fi
if command -v nm >/dev/null 2>&1; then
    for symbol in hda_driver_module ac97_driver_module es1371_driver_module \
                  e1000_driver_module mouse_driver_module serial_driver_module; do
        if ! nm "$audio_out/generated/system/kernel.debug" | grep -q " $symbol\$"; then
            echo "FAIL kernel.debug does not contain builtin driver symbol: $symbol" >&2
            exit 1
        fi
    done
else
    echo "FAIL nm is required to verify builtin driver symbols" >&2
    exit 1
fi
python3 - "$audio_out/rootfs/manifest.json" <<'PY'
import json,sys
manifest=json.load(open(sys.argv[1]))
text=json.dumps(manifest)
assert 'usr/lib/reliefos/drivers' not in text, 'staged driver directory in manifest'
assert '.drv' not in text, 'staged .drv driver module in manifest'
assert 'usr/lib/reliefos/kerneldebug.sys' in text
print('Builtin driver products and staged manifest PASS')
PY
