#!/usr/bin/env python3
"""Test real SysV message queues and native Linux calls without building an ISO."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefos-sysv-msg-") as directory:
    output = str(Path(directory) / "sysv_msg")
    subprocess.run(["cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all", "-ffunction-sections", "-fdata-sections",
                    "-Wl,--gc-sections", "-fno-pie", "-no-pie", "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi",
                    "-Ikernel/reliefnt/include", "tools/tests/sysv_msg_test.c", "-o", output],
                   cwd=root, check=True)
    subprocess.run([output], cwd=root, check=True, timeout=20)
    output = str(Path(directory) / "sysv_msg_abi")
    subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra",
                    "tools/tests/sysv_msg_abi_test.c", "-o", output], cwd=root, check=True)
    subprocess.run([output], cwd=root, check=True, timeout=20)
