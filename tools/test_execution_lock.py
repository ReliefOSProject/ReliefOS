#!/usr/bin/env python3
"""Test the real execution gate with simulated per-CPU IRQ state."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory(prefix="reliefos-execution-lock-") as tmp:
    binary = str(Path(tmp) / "test")
    subprocess.run(["cc", "-std=c11", "-pthread", "-O2", "-g", "-Wall", "-Wextra",
                    "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                    "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/include",
                    "tools/tests/execution_lock_test.c", "kernel/reliefnt/kernel/sync/lock.c",
                    "-o", binary], cwd=ROOT, check=True)
    subprocess.run([binary], cwd=ROOT, check=True, timeout=30)
