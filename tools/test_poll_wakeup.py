#!/usr/bin/env python3
"""Build and run the host poll/AF_UNIX readiness wakeup fixture."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefos-poll-wakeup-") as tmp:
    binary = Path(tmp) / "poll-wakeup"
    subprocess.run([
        "cc", "-std=c11", "-g", "-O1", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", "-ffunction-sections", "-fdata-sections",
        "-Wl,--gc-sections", "-fno-pie", "-no-pie",
        "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi",
        "-Ikernel/reliefnt/kernel/reliefnt/include",
        "tools/tests/poll_wakeup_test.c", "kernel/reliefnt/kernel/reliefnt/syscall.c",
        "kernel/reliefnt/kernel/reliefnt/wait.c", "-o", str(binary),
    ], cwd=ROOT, check=True)
    subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=20)
