#!/usr/bin/env python3
"""Verify the metadata boundary using an ext2 image made/inspected by e2fsprogs."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefos-storage-") as directory:
    temp = Path(directory)
    disk, source, executable = temp / "disk.ext2", temp / "input", temp / "metadata"
    source.write_bytes(b"hello")
    source.chmod(0o644)
    subprocess.run(["mke2fs", "-q", "-t", "ext2", "-b", "1024", "-I", "128", "-O", "none",
                    "-F", disk, "8192"], check=True)
    subprocess.run(["debugfs", "-w", "-R", f"write {source} /probe", disk], check=True, capture_output=True)
    # debugfs imports the host UID. Set the fixture's initial ownership explicitly.
    for field in ("uid", "gid"):
        subprocess.run(["debugfs", "-w", "-R", f"set_inode_field /probe {field} 0", disk], check=True, capture_output=True)
    before = subprocess.check_output(["debugfs", "-R", "stat /probe", disk], text=True)
    inode = re.search(r"Inode:\s*(\d+)", before).group(1)
    subprocess.run(["cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                    "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/include",
                    "tools/tests/storage_metadata_test.c", "kernel/reliefnt/fs/tmpfs/core.c", "-o", executable], cwd=ROOT, check=True)
    subprocess.run([executable, disk, inode], cwd=ROOT, check=True)
    after = subprocess.check_output(["debugfs", "-R", "stat /probe", disk], text=True)
    assert re.search(r"Mode:\s*06750", after), after
    assert re.search(r"User:\s*70001\s+Group:\s*90002", after), after
    print("PASS e2fsprogs independently reads persisted mode 6750 and UID:GID 70001:90002")
