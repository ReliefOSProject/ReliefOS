#!/usr/bin/env python3
"""Exercise the installer's production read aggregation with actual file data."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="installer-copy-", dir=ROOT / "build") as directory:
    binary = Path(directory) / "copy"
    subprocess.run(["cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g",
                    "-fsanitize=address,undefined", "-Wl,--wrap=read",
                    "tools/tests/installer_copy_read_test.c", "-o", binary], cwd=ROOT, check=True)
    subprocess.run([binary, Path(directory) / "input.bin"], check=True, timeout=10)
