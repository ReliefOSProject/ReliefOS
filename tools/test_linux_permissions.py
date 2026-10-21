#!/usr/bin/env python3
"""Exercise the real kernel permission checks and persistent metadata runtime."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory(prefix="reliefos-permissions-") as directory:
    executable = str(Path(directory) / "permissions")
    subprocess.run(["cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-ffunction-sections", "-fdata-sections",
                    "-Wl,--gc-sections", "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/include",
                    "tools/tests/linux_permissions_test.c", "-o", executable], cwd=ROOT, check=True)
    subprocess.run([executable], cwd=ROOT, check=True, timeout=30)
