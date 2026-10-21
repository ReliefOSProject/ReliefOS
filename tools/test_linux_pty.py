#!/usr/bin/env python3
"""Exercise the real kernel PTY implementation with native Linux encodings."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
binary = ROOT / "build/linux-pty-test"
binary.parent.mkdir(parents=True, exist_ok=True)
subprocess.run([
    "clang", "-std=c11", "-g", "-O1", "-ffunction-sections", "-fdata-sections",
    "-fsanitize=address,undefined", "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi",
    "-Ikernel/reliefnt/include", "-Wl,--gc-sections",
    "tools/tests/linux_pty_test.c", "-o", str(binary),
], cwd=ROOT, check=True)
subprocess.run([str(binary)], check=True)
