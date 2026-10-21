#!/usr/bin/env python3
"""Exercise kernel SysV semaphores and native Linux ABI without virtual machines."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefos-sysv-sem-") as directory:
    output = str(Path(directory) / "sysv_sem")
    subprocess.run(["cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all", "-ffunction-sections", "-fdata-sections",
                    "-Wl,--gc-sections", "-fno-pie", "-no-pie", "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi",
                    "-Ikernel/reliefnt/include", "tools/tests/sysv_sem_test.c", "-o", output],
                   cwd=root, check=True)
    subprocess.run([output], cwd=root, check=True, timeout=20)
    output = str(Path(directory) / "sysv_sem_abi")
    subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra",
                    "tools/tests/sysv_sem_abi_test.c", "-o", output], cwd=root, check=True)
    subprocess.run([output], cwd=root, check=True, timeout=20)
