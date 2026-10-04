#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
audio_out=${AUDIO_OUT:-"$repo/out/audio-hda"}
audio_root="$audio_out/rootfs/raw"
for name in hda ac97 es1371 e1000 mouse serial; do
    for product in "$audio_out/generated/drivers/$name.drv" "$audio_out/kernel-install/$name.drv" "$audio_root/usr/lib/reliefos/drivers/$name.drv"; do
        if [ ! -s "$product" ]; then
            echo "FAIL missing product: $product" >&2
            exit 1
        fi
    done
    cmp "$audio_out/generated/drivers/$name.drv" "$audio_out/kernel-install/$name.drv"
    cmp "$audio_out/generated/drivers/$name.drv" "$audio_root/usr/lib/reliefos/drivers/$name.drv"
done
python3 - "$audio_out/rootfs/manifest.json" <<'PY'
import json,sys
manifest=json.load(open(sys.argv[1]))
text=json.dumps(manifest)
for name in ('hda','ac97','es1371','e1000','mouse','serial'):
    assert 'usr/lib/reliefos/drivers/'+name+'.drv' in text, name
print('HDA and five legacy module products/staged manifest PASS')
PY
