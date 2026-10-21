#!/usr/bin/env python3
"""Exercise production record locks with sanitizer and injected allocation failure."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefos-record-locks-") as directory:
    binary = str(Path(directory) / "locks")
    subprocess.run(["cc", "-std=c11", "-O2", "-g", "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all", "-fno-pie", "-no-pie",
                    "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/include",
                    "tools/tests/record_locks_test.c", "-o", binary], cwd=ROOT, check=True)
    subprocess.run([binary], cwd=ROOT, check=True, timeout=30)
