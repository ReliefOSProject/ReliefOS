#!/usr/bin/env python3
"""Check ext2 batched allocation and expose the native ext4 write fixture."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--filesystem", choices=("ext2", "ext4"), default="ext2")
args = parser.parse_args()
if args.filesystem == "ext4":
    subprocess.run(["python3", str(ROOT / "tools/test_ext4_write.py")], cwd=ROOT, check=True)
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix="ext2-write-batch-", dir=ROOT / "build") as directory:
    work = Path(directory)
    executable = work / "batch"
    subprocess.run(["cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g",
                    "-fsanitize=address,undefined", "-ffunction-sections", "-fdata-sections",
                    "-Wl,--gc-sections", "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/include",
                    "tools/tests/ext2_write_batch_test.c", "-o", executable], cwd=ROOT, check=True)
    for bs in (1024, 2048, 4096):
        for failure in range(7):
            image = work / f"disk-{bs}-{failure}.ext2"
            subprocess.run(["mke2fs", "-q", "-t", "ext2", "-b", str(bs), "-I", "128",
                            "-g", "1024", "-O", "none,filetype", "-F", image, "16384"], check=True)
            subprocess.run([executable, image, str(failure)], check=True, timeout=30)
            subprocess.run(["e2fsck", "-f", "-n", image], check=True)
