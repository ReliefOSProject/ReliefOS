#!/usr/bin/env python3
"""Exercise the real storage VFS and ext2 backend at installer mount boundaries."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefos-mkdir-mount-") as directory:
    work = Path(directory)
    image, executable = work / "root.ext2", work / "mkdir-mount"
    subprocess.run(["mke2fs", "-q", "-t", "ext2", "-b", "1024", "-I", "128",
                    "-O", "none", "-F", image, "8192"], check=True)
    subprocess.run(["cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                    "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                    "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/include",
                    "tools/tests/storage_mkdir_mount_test.c", "kernel/reliefnt/fs/tmpfs/core.c",
                    "kernel/reliefnt/lib/text_utf16.c", "-o", executable],
                   cwd=ROOT, check=True)
    subprocess.run([executable, image], cwd=ROOT, check=True, timeout=30)
