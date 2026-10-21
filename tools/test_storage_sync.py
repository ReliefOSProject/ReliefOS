#!/usr/bin/env python3
"""Test real storage sync routing and ATA command submission with fault injection."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefos-storage-sync-") as directory:
    binary = str(Path(directory) / "sync")
    subprocess.run(["cc", "-std=c11", "-O2", "-g", "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all", "-fno-pie", "-no-pie",
                    "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                    "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/include",
                    "tools/tests/storage_sync_test.c", "-o", binary], cwd=ROOT, check=True)
    subprocess.run([binary], cwd=ROOT, check=True, timeout=30)
