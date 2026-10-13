#!/usr/bin/env python3
"""Test native file mutations against independent e2fsprogs checks and reads."""
import argparse
from pathlib import Path
import re
import tempfile
from test_ext4_read import ROOT, STORAGE, FS, run

def build(work):
    binary = work / "write-test"
    run(["cc", "-std=c11", "-O1", "-g", "-D_GNU_SOURCE", "-fsanitize=address,undefined",
         "-fno-sanitize-recover=all", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
         "-DRELIEFOS_STORAGE_STANDALONE_TU", "-Ikernel/reliefnt/include", "-Iinclude",
         "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/kernel/reliefnt/include",
         "-include", str(STORAGE / "storage_internal.h"), "tools/tests/ext4_write_test.c",
         *[str(FS / f"storage_ext4_{n}.c") for n in
           ("format", "checksum", "cache", "alloc", "extent", "ops", "xattr", "journal")], "-o", str(binary)])
    return binary

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", default="all", choices=("all",))
    parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="reliefos-ext4-write-") as d:
        work = Path(d); binary = build(work); stage = work / "stage"; stage.mkdir()
        (stage / "payload").touch()
        for bs in (1024, 2048, 4096):
            for kind, features in (("ext2", "none,filetype,large_file"),
                ("ext4", "none,filetype,extents,64bit,flex_bg,metadata_csum,extra_isize,has_journal")):
                image = work / f"{kind}-{bs}.img"
                with image.open("wb") as f: f.truncate(64*1024*1024)
                run(["mke2fs", "-q", "-F", "-t", kind, "-b", str(bs), "-I", "256", "-O", features,
                     "-d", str(stage), str(image)])
                stat = run(["debugfs", "-R", "stat /payload", str(image)], capture_output=True)
                ino = re.search(r"Inode:\s+(\d+)", stat.stdout)[1]
                result=work/"result.img"; expected=work/"expected"; actual=work/"actual"
                run([str(binary), str(image), ino, str(result), str(expected)])
                run(["e2fsck", "-f", "-n", str(result)])
                run(["debugfs", "-R", f"dump /payload {actual}", str(result)], capture_output=True)
                assert actual.read_bytes() == expected.read_bytes()
                print(f"PASS independent Linux image oracle {kind} block={bs}", flush=True)
    print("PASS ext4 file mutation suite")

if __name__ == "__main__": main()
