#!/usr/bin/env python3
"""Exercise the real block layer through a sector-accurate AHCI transport fixture."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="storage-write-batch-", dir=ROOT / "build") as directory:
    binary = Path(directory) / "batch"
    subprocess.run(["cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                    "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                    "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/include",
                    "tools/tests/storage_write_batch_test.c", "-o", binary], cwd=ROOT, check=True)
    subprocess.run([binary], check=True, timeout=30)
