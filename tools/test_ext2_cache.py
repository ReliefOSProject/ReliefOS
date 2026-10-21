#!/usr/bin/env python3
"""Test clean ext2 caching and raw-device invalidation using the real driver."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="ext2-cache-", dir=ROOT / "build") as directory:
    work = Path(directory)
    executable = work / "cache"
    subprocess.run(["cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                    "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                    "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/include",
                    "tools/tests/ext2_cache_test.c", "-o", executable], cwd=ROOT, check=True)
    for block_size in (1024, 2048, 4096):
        image = work / f"disk-{block_size}.ext2"
        subprocess.run(["mke2fs", "-q", "-t", "ext2", "-b", str(block_size), "-I", "128",
                        "-O", "none,filetype", "-F", image, "8192"], check=True)
        subprocess.run([executable, image], check=True, timeout=30)
