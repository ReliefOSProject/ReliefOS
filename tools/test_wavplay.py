#!/usr/bin/env python3
"""Exercise the userspace wavplay short-write/retry loop."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="wavplay-") as work:
    binary = Path(work) / "wavplay"
    subprocess.run(["cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g", "-no-pie", "-pthread",
        "-fsanitize=address,undefined", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-Ikernel/reliefnt/include/uapi", "-Iuserland/runtime/include", "-Iinclude",
        "-Wall", "-Wextra", "-Werror", "tools/tests/wavplay_retry_test.c", "-o", str(binary)], cwd=ROOT, check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
