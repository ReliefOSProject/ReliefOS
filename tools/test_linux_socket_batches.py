#!/usr/bin/env python3
"""Run native Linux socket batch contracts and the kernel's batch state machine."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefos-mmsg-") as directory:
    output = str(Path(directory) / "socket_batch")
    subprocess.run(["cc", "-std=c11", "-g", "-O1", "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all", "-ffunction-sections", "-fdata-sections",
                    "-Wl,--gc-sections", "-fno-pie", "-no-pie", "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi",
                    "-Ikernel/reliefnt/include", "tools/tests/socket_batch_test.c", "-o", output],
                   cwd=root, check=True)
    subprocess.run([output], cwd=root, check=True, timeout=20)
    output = str(Path(directory) / "socket_batch_unix")
    subprocess.run(["cc", "-std=c11", "-g", "-O1", "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all", "-ffunction-sections", "-fdata-sections",
                    "-Wl,--gc-sections", "-fno-pie", "-no-pie", "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi",
                    "-Ikernel/reliefnt/include", "tools/tests/socket_batch_unix_test.c", "-o", output],
                   cwd=root, check=True)
    subprocess.run([output], cwd=root, check=True, timeout=20)
    output = str(Path(directory) / "socket_batch_abi")
    subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra",
                    "tools/tests/socket_batch_abi_test.c", "-o", output], cwd=root, check=True)
    subprocess.run([output], cwd=root, check=True, timeout=20)
