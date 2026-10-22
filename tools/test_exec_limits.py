#!/usr/bin/env python3
"""Linux reference run for the execve probe also compiled inside ReliefOS."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TMP = ROOT / 'out/tmp'
TMP.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='exec-limits-', dir=TMP) as directory:
    for fixture in ('exec_task_params', 'exec_stack'):
        owned = Path(directory) / fixture
        subprocess.run([
            'clang', '-std=c11', '-O1', '-g', '-ffunction-sections', '-fdata-sections',
            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
            '-Ikernel/reliefnt/include', '-Ikernel/reliefnt/include/uapi', '-Iinclude',
            f'tools/tests/{fixture}_test.c', '-Wl,--gc-sections', '-o', str(owned),
        ], cwd=ROOT, check=True)
        subprocess.run([str(owned)], cwd=ROOT, check=True, timeout=30)
    binary = Path(directory) / 'exec-limits'
    subprocess.run([
        'cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
        str(ROOT / 'tools/tests/exec_limits_runtime_probe.c'), '-o', str(binary),
    ], cwd=ROOT, env={**os.environ, 'TMPDIR': str(TMP)}, check=True)
    subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=60)
