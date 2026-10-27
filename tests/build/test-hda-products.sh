#!/bin/sh
# Verify actual configured kernel products and their complete staged bundle.
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
for product in "$audio_out/generated/system/kerneldebug.sys" "$audio_out/kernel-install/kerneldebug.sys"; do
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
kernel_tools=${RELIEFOS_KERNEL_TOOLS:-"$audio_out/host/bin/reliefos-kernel-tools"}
inspector=${RELIEFOS_TEST_RECOVERY:-"$audio_out/userland/initramfs/kernel-bundle.elf"}
"$kernel_tools" check-install "$audio_out/kernel-install"
python3 - "$audio_out" <<'PY'
import json,sys
from pathlib import Path
out=Path(sys.argv[1]);install=out/'kernel-install';root=out/'rootfs/raw'
bundle=json.loads((install/'install.bundle.json').read_bytes())
release=bundle['release'];configuration=bundle['configuration']
module_names={Path(name).name for name in bundle['modules']}
builtin_names={Path(name).name for name in (install/'lib/modules'/release/'modules.builtin').read_text().splitlines()}
for option,name in {'SND_HDA_INTEL':'hda','SND_INTEL8X0':'ac97','SND_ENS1371':'es1371',
                    'E1000':'e1000','MOUSE_PS2':'mouse','SERIAL_8250':'serial'}.items():
    state=configuration.get(option,'n');product=name+'.ko'
    assert (product in module_names)==(state=='m'), (option,state,'module inventory')
    assert (product in builtin_names)==(state=='y'), (option,state,'builtin inventory')
    print(f'{option}={state}: {product}')
for name in bundle['modules']:
    assert (root/name).read_bytes()==(install/name).read_bytes(),name
payload=json.loads((root/'usr/lib/reliefos/kernel'/release/'bundle.json').read_bytes())
assert payload['release']==release and payload['module_abi']==bundle['module_abi']
assert (root/'usr/lib/reliefos/kernel'/release/'kerneldebug.sys').is_file()
manifest=json.loads((out/'rootfs/manifest.json').read_bytes());text=json.dumps(manifest)
assert 'usr/lib/reliefos/drivers' not in text, 'staged driver directory in manifest'
assert '.drv' not in text, 'staged .drv driver module in manifest'
assert f'usr/lib/reliefos/kernel/{release}/kerneldebug.sys' in text
print('Configured kernel products and staged manifest PASS')
PY
release=$(cat "$audio_out/rootfs/raw/usr/lib/reliefos/kernel/current-release")
"$inspector" --inspect "$audio_root" "$release"
