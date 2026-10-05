#!/usr/bin/env python3
"""Exercise actual native nanosleep state without a libc adapter."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefos-time-") as tmp:
    output = Path(tmp) / "time"
    subprocess.run(["cc", "-std=c11", "-g", "-O1", "-fsanitize=address,undefined",
                    "-Wall", "-Wextra", "-Werror", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                    "-fno-pie", "-no-pie", "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi",
                    "-Ikernel/reliefnt/kernel/reliefnt/include", "tools/tests/nanosleep_state_test.c",
                    "-o", output], cwd=root, check=True)
    subprocess.run([output], cwd=root, check=True, timeout=20)
