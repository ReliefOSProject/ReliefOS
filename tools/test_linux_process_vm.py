#!/usr/bin/env python3
"""Exercise process_vm with actual page tables and compare raw calls on Linux."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefos-process-vm-") as directory:
    output = str(Path(directory) / "process_vm")
    subprocess.run(["cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all", "-ffunction-sections", "-fdata-sections",
                    "-Wl,--gc-sections", "-fno-pie", "-no-pie", "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi",
                    "-Ikernel/reliefnt/include", "tools/tests/process_vm_test.c", "-o", output],
                   cwd=root, check=True)
    subprocess.run([output], cwd=root, check=True, timeout=20)
    output = str(Path(directory) / "process_vm_abi")
    subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra",
                    "tools/tests/process_vm_abi_test.c", "-o", output], cwd=root, check=True)
    subprocess.run([output], cwd=root, check=True, timeout=20)
