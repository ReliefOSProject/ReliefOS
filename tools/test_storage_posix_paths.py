#!/usr/bin/env python3
"""Check XDM-compatible colon pathnames against the real ext2/ext4 storage code."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(command):
    subprocess.run(command, cwd=ROOT, check=True, timeout=60)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--filesystem", choices=("ext2", "ext4", "both"), default="both")
    parser.add_argument("--backend-only", action="store_true")
    args = parser.parse_args()
    filesystems = ("ext2", "ext4") if args.filesystem == "both" else (args.filesystem,)
    with tempfile.TemporaryDirectory(prefix="reliefos-posix-paths-") as directory:
        work = Path(directory)
        binary = work / "paths"
        run(["cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
             "-fno-sanitize-recover=all", "-ffunction-sections", "-fdata-sections",
             "-Wl,--gc-sections", "-Ikernel/reliefnt/include", "-Iinclude",
             "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/kernel/reliefnt/include",
             "tools/tests/storage_posix_paths_test.c", "kernel/reliefnt/fs/tmpfs.c",
             "kernel/reliefnt/kernel/reliefnt/lib/text_utf16.c", "-o", str(binary)])
        for filesystem in filesystems:
            image = work / (filesystem + ".img")
            with image.open("wb") as stream:
                stream.truncate(64 * 1024 * 1024)
            features = "none,filetype,large_file,dir_index"
            if filesystem == "ext4":
                features += ",extents,64bit,metadata_csum,extra_isize,flex_bg,has_journal"
            run(["mke2fs", "-q", "-F", "-t", filesystem, "-b", "4096", "-I", "256",
                 "-O", features, str(image)])
            command = [str(binary), str(image)]
            if args.backend_only:
                command.append("--backend-only")
            run(command)
            print(f"storage-posix-paths {filesystem}: PASS", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
