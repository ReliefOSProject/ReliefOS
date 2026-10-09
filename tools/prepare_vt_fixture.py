#!/usr/bin/env python3
"""Create an exclusive installed-image copy containing the real VT ABI probe."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def prepare(source, output, text_only=False, compiler=None,
            probe_source='tools/tests/vt_guest_test.c'):
    output.mkdir(parents=True, exist_ok=False)
    probe = output / 'vt-probe'
    if compiler is None:
        compiler = ROOT / 'out/x86_64/release/sdk/reliefos-musl-sdk/bin/reliefos-musl-cc'
    subprocess.run([str(compiler), '-D_GNU_SOURCE', '-static', '-Iinclude', '-Ikernel/reliefnt/include/uapi',
                    probe_source, '-o', str(probe)], cwd=ROOT, check=True)
    disk = output / 'disk.raw'
    subprocess.run(['cp', '--reflink=auto', '--sparse=always', str(source), str(disk)], check=True)
    layout = json.loads(subprocess.check_output(['sfdisk', '--json', str(disk)]))['partitiontable']
    root = layout['partitions'][1]
    offset, length = root['start'] * layout['sectorsize'], root['size'] * layout['sectorsize']
    fs = output / 'root.ext2'
    with disk.open('rb') as src, fs.open('wb') as dst:
        src.seek(offset)
        remaining = length
        while remaining:
            chunk = src.read(min(16 * 1024**2, remaining))
            if not chunk: raise RuntimeError('Truncated root partition')
            dst.write(chunk)
            remaining -= len(chunk)
    commands = [f'write {probe.resolve()} /tmp/vt-probe', 'set_inode_field /tmp/vt-probe mode 0100755']
    if text_only: commands.append('rm /etc/reliefos/desktop-backend')
    for command in commands:
        subprocess.run(['debugfs', '-w', '-R', command, str(fs)], check=True)
    with fs.open('rb') as src, disk.open('r+b') as dst:
        dst.seek(offset)
        shutil.copyfileobj(src, dst, 16 * 1024**2)
    return disk

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=ROOT / 'out/x86_64/release/images/reliefos.raw')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--text-only', action='store_true')
    parser.add_argument('--compiler', type=Path, default=None,
                        help='reliefos-musl-cc to build the probe with '
                             '(default: out/x86_64/release/sdk/reliefos-musl-sdk/bin/reliefos-musl-cc)')
    parser.add_argument('--probe-source', default='tools/tests/vt_guest_test.c',
                        help='guest probe C source to install as /tmp/vt-probe')
    args = parser.parse_args()
    print(prepare(args.image, args.output, args.text_only, args.compiler, args.probe_source))
