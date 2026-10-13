#!/usr/bin/env python3
"""Sanitized extent/indirect reads, including independent e2fsprogs images."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
STORAGE = ROOT / "kernel/reliefnt/block"
FS = ROOT / "kernel/reliefnt/fs"


def run(args, **kwargs):
    return subprocess.run(args, cwd=ROOT, check=True, text=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", choices=("all", "unit", "images"), default="all")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="reliefos-ext4-read-") as directory:
        work = Path(directory)
        binary = work / "read-test"
        sources = [FS / f"storage_ext4_{name}.c" for name in
                   ("format", "checksum", "cache", "alloc", "extent", "ops", "xattr", "journal")]
        run(["cc", "-std=c11", "-O1", "-g", "-D_GNU_SOURCE",
             "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
             "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
             "-DRELIEFOS_STORAGE_STANDALONE_TU", "-Ikernel/reliefnt/include",
             "-Iinclude", "-Ikernel/reliefnt/include/uapi",
             "-Ikernel/reliefnt/kernel/reliefnt/include", "-include",
             str(STORAGE / "storage_internal.h"),
             str(ROOT / "tools/tests/ext4_extent_test.c"), *map(str, sources),
             "-o", str(binary)])
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1",
                   UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        vfs_binary = work / "vfs-read-test"
        if args.case in ("all", "images"):
            run(["cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                 "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                 "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi",
                 "-Ikernel/reliefnt/kernel/reliefnt/include", "tools/tests/ext4_vfs_read_test.c",
                 "kernel/reliefnt/fs/tmpfs.c",
                 "-o", str(vfs_binary)])
        if args.case in ("all", "unit"):
            run([str(binary)], env=env)
        if args.case in ("all", "images"):
            stage = work / "stage"
            stage.mkdir()
            # 4 MiB crosses double-indirect at 1K and single-indirect at 4K.
            payload = bytes((i * 29 + 7) % 251 for i in range(65536)) * 64
            (stage / "payload").write_bytes(payload)
            sparse = stage / "sparse"
            with sparse.open("wb") as f:
                f.write(payload[:1024])
                f.seek(3 * 1024 * 1024)
                f.write(payload[-1024:])
            with (stage / "fragmented").open("wb") as f:
                for i in range(1000):
                    f.seek(i * 8192)
                    f.write(payload[:512])
            for block in (1024, 2048, 4096):
                for kind, features in (("ext2", "none,filetype,large_file"),
                                       ("ext4", "none,filetype,extents,64bit,flex_bg,metadata_csum,extra_isize")):
                    image = work / f"{kind}-{block}.img"
                    image.write_bytes(b"")
                    with image.open("r+b") as f:
                        f.truncate(64 * 1024 * 1024)
                    run(["mke2fs", "-q", "-F", "-t", kind, "-b", str(block),
                         "-I", "256", "-O", features, "-d", str(stage), str(image)])
                    for name in ("payload", "sparse", "fragmented"):
                        result = run(["debugfs", "-R", f"stat /{name}", str(image)],
                                     capture_output=True)
                        match = re.search(r"Inode:\s+(\d+)", result.stdout)
                        if not match:
                            raise RuntimeError(result.stdout + result.stderr)
                        run([str(binary), str(image), match[1], str(stage / name)], env=env)
                        if kind == "ext4":
                            run([str(vfs_binary), str(image), match[1], str(stage / name)], env=env)
                    run(["e2fsck", "-f", "-n", str(image)], capture_output=True)
    print("PASS ext4 extent/indirect read suite")


if __name__ == "__main__":
    main()
