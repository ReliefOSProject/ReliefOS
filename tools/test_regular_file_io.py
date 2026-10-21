#!/usr/bin/env python3
"""Exercise the real syscall transfer engine with controlled backend completion."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="regular-file-io-") as directory:
    binary = Path(directory) / "test"
    subprocess.run(["clang", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all", "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi",
                    "-Ikernel/reliefnt/include", "tools/tests/regular_file_io_test.c",
                    "-o", str(binary)], cwd=ROOT, check=True)
    subprocess.run([str(binary)], check=True, timeout=20)
