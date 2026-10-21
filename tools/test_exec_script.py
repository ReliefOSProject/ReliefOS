#!/usr/bin/env python3
"""Exercise production script parsing and argv updates under ASan/UBSan."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
binary = ROOT / "build/exec-script-test"
binary.parent.mkdir(parents=True, exist_ok=True)
subprocess.run([
    "clang", "-std=c11", "-g", "-O1", "-ffunction-sections", "-fdata-sections",
    "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi",
    "-Ikernel/reliefnt/include", "-Wl,--gc-sections",
    "tools/tests/exec_script_test.c", "-o", str(binary),
], cwd=ROOT, check=True)
subprocess.run([str(binary)], check=True, timeout=30)
