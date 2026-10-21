#!/usr/bin/env python3
"""Run production capability dispatch under sanitizers, without host capset."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefos-capabilities-") as directory:
    binary = str(Path(directory) / "capabilities")
    subprocess.run([
        "cc", "-std=c11", "-O2", "-g", "-flto", "-fwhole-program",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-pie", "-no-pie",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/include",
        "tools/tests/capability_test.c", "-o", binary,
    ], cwd=ROOT, check=True)
    subprocess.run([binary], cwd=ROOT, check=True, timeout=30)
