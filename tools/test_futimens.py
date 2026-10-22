#!/usr/bin/env python3
"""Test production fd timestamp handling and its Linux syscall reference."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TMP = ROOT / 'out/tmp'
TMP.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='futimens-', dir=TMP) as directory:
    for name, flags in (
        ('futimens_test', ['-O1', '-g', '-ffunction-sections', '-fdata-sections',
                          '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                          '-Ikernel/reliefnt/include', '-Ikernel/reliefnt/include/uapi',
                          '-Iinclude', '-Wl,--gc-sections']),
        ('futimens_runtime_probe', ['-O2', '-Wall', '-Wextra', '-Werror']),
    ):
        binary = Path(directory) / name
        subprocess.run(['clang', '-std=c11', *flags, f'tools/tests/{name}.c', '-o', str(binary)],
                       cwd=ROOT, env={**os.environ, 'TMPDIR': str(TMP)}, check=True)
        subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=30)
