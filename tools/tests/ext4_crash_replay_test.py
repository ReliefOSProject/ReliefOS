#!/usr/bin/env python3
"""Real driver crash injection plus independent e2fsck journal replay."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=ROOT, check=True, text=True, **kw)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("host-fixture",), default="host-fixture")
    parser.add_argument("--iterations", type=int, default=1)
    args = parser.parse_args()
    if args.iterations < 1:
        parser.error("iterations must be positive")
    with tempfile.TemporaryDirectory(prefix="reliefos-jbd2-") as directory:
        work = Path(directory)
        binary = work / "journal-test"
        storage = ROOT / "kernel/reliefnt/drivers/bootstrap/storage"
        fs = ROOT / "kernel/reliefnt/fs"
        run(["cc", "-std=c11", "-O1", "-g", "-D_GNU_SOURCE",
             "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
             "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
             "-DRELIEFOS_STORAGE_STANDALONE_TU", "-Ikernel/reliefnt/include", "-Iinclude",
             "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/kernel/reliefnt/include",
             "-include", str(storage / "storage_internal.h"),
             "tools/tests/ext4_journal_test.c",
             *[str(fs / f"storage_ext4_{name}.c") for name in
               ("format", "checksum", "cache", "alloc", "extent", "ops", "xattr", "journal")],
             "-o", str(binary)])
        vfs = work / "vfs-read"
        run(["cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
             "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
             "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi",
             "-Ikernel/reliefnt/kernel/reliefnt/include", "tools/tests/ext4_vfs_read_test.c",
             "kernel/reliefnt/fs/tmpfs.c", "-o", str(vfs)])
        stage = work / "stage"
        stage.mkdir()
        (stage / "payload").write_bytes(b"original" * 1024)
        for bs in (1024, 4096):
            image = work / f"base-{bs}.img"
            with image.open("wb") as f:
                f.truncate(32 * 1024 * 1024)
            run(["mke2fs", "-q", "-F", "-t", "ext4", "-b", str(bs), "-I", "256",
                 "-O", "none,filetype,extents,64bit,has_journal,metadata_csum,extra_isize",
                 "-d", str(stage), str(image)])
            stat = run(["debugfs", "-R", "stat /payload", str(image)], capture_output=True)
            match = re.search(r"Inode:\s+(\d+)", stat.stdout)
            if not match:
                raise RuntimeError(stat.stdout + stat.stderr)
            for profile in (0, 1, 8, 10, 16, 18):
                for iteration in range(args.iterations):
                    committed = work / f"committed-{bs}-{profile}.img"
                    run([str(binary), str(image), match[1], str(profile), str(committed)],
                        env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1",
                                 UBSAN_OPTIONS="halt_on_error=1"))
                    run(["e2fsck", "-f", "-n", str(committed) + ".clean"], capture_output=True)
                    expected = work / "expected-replay"
                    expected.write_bytes(bytes.fromhex("c03b3998") + b"c" * (bs - 4)
                                         + (stage / "payload").read_bytes()[bs:])
                    run([str(vfs), str(committed), match[1], str(expected)])
                    check = subprocess.run(["e2fsck", "-f", "-y", str(committed)],
                                           capture_output=True, text=True)
                    if check.returncode not in (0, 1):
                        raise RuntimeError(check.stdout + check.stderr)
                    run(["e2fsck", "-f", "-n", str(committed)], capture_output=True)
                    dump = work / "dump"
                    dump.unlink(missing_ok=True)
                    run(["debugfs", "-R", f"dump /payload {dump}", str(committed)], capture_output=True)
                    assert dump.read_bytes()[:bs] == bytes.fromhex("c03b3998") + b"c" * (bs - 4)
                    print(f"PASS e2fsck independent replay bs={bs} profile={profile} iteration={iteration}", flush=True)


if __name__ == "__main__":
    main()
