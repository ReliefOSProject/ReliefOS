#!/usr/bin/env python3
"""Build the native SysV SHM fixture and canonical ABI probe."""
from pathlib import Path
import hashlib
import json
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / 'tools/tests/fixtures/sysv-shm-uapi/linux-v6.14'
for entry in json.loads((FIXTURE / 'manifest.json').read_text()):
    assert hashlib.sha256((FIXTURE / entry['path']).read_bytes()).hexdigest() == entry['sha256'], entry['path']
COMMON = ["-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi",
          "-Ikernel/reliefnt/kernel/reliefnt/include"]

with tempfile.TemporaryDirectory(prefix="reliefos-sysv-shm-") as directory:
    output = str(Path(directory) / "sysv_shm")
    subprocess.run(["cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all", "-ffunction-sections", "-fdata-sections",
                    "-Wl,--gc-sections", "-fno-pie", "-no-pie", *COMMON,
                    "tools/tests/sysv_shm_test.c", "-o", output], cwd=ROOT, check=True)
    subprocess.run([output], cwd=ROOT, check=True, timeout=20)
    layouts = []
    for name, includes in [('target', COMMON), ('reference', ['-I' + str(FIXTURE), *COMMON])]:
        output = str(Path(directory) / ('sysv_shm_abi_' + name))
        subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", *includes,
                        "tools/tests/sysv_shm_abi_test.c", "-o", output], cwd=ROOT, check=True)
        layouts.append(subprocess.check_output([output], cwd=ROOT, text=True, timeout=20))
    assert layouts[0] == layouts[1], 'canonical SHM layouts/flags differ'
    print('PASS SysV SHM canonical Linux v6.14: LP64 layouts, field offsets and all public IPC/SHM values')
