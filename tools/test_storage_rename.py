"""Exercise the real ext2 rename implementation and inspect its disk result."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def check_image(directory, features, block_size, ram_root=False):
    temp = Path(directory)
    disk, source, target, executable = (temp / name for name in ("disk.ext2", "source", "target", "rename"))
    source.write_bytes(b"new")
    source.chmod(0o640)
    target.write_bytes(b"old data")
    subprocess.run(["mke2fs", "-q", "-t", "ext2", "-b", str(block_size), "-I", "128", "-O", features,
                    "-F", disk, "8192"], check=True)
    for command in (f"write {source} /source", f"write {target} /target", "mkdir /left",
                    "mkdir /right", f"write {target} /right/child"):
        subprocess.run(["debugfs", "-w", "-R", command, disk], check=True, capture_output=True)
    subprocess.run(["cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                    "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/include",
                    "tools/tests/storage_rename_test.c", "kernel/reliefnt/fs/tmpfs/core.c", "-o", executable], cwd=ROOT, check=True)
    subprocess.run([executable, disk, *(("--ram-root",) if ram_root else ())], check=True)
    content = subprocess.check_output(["debugfs", "-R", "cat /target", disk])
    assert content == b"new", content
    socket_stat = subprocess.check_output(["debugfs", "-R", "stat /socket-persisted", disk], text=True)
    assert "Type: socket" in socket_stat, socket_stat
    subprocess.run(["e2fsck", "-f", "-n", disk], check=True)
    print("PASS debugfs replacement data and e2fsck consistency")


for features, block_size, ram_root in (("none", 1024, False), ("none,filetype", 1024, False),
                                      ("none,filetype", 4096, False), ("none,filetype", 4096, True)):
    with tempfile.TemporaryDirectory(prefix="reliefos-rename-") as directory:
        check_image(directory, features, block_size, ram_root)

with tempfile.TemporaryDirectory(prefix="linux-symlink-reference-") as directory:
    executable = Path(directory) / "symlink"
    subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra",
                    "tools/tests/symlink_abi_test.c", "-o", executable], cwd=ROOT, check=True)
    subprocess.run([executable], check=True)
