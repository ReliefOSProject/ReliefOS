#!/usr/bin/env python3
"""Exercise both directions of the Linux/e4fsprogs <-> ReliefOS contract.

The fixture deliberately uses ordinary e2fsprogs images as the oracle.  The
native readers and writers are compiled with the same standalone storage
sources used by the other host tests, so a passing run proves that the image
was not merely inspected by e2fsprogs itself.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import subprocess
import tempfile
from pathlib import Path

from ext4_fixture import create_image, e2fsck_read_only, run_checked

ROOT = Path(__file__).resolve().parents[2]
STORAGE = ROOT / "kernel/reliefnt/block"
FS = ROOT / "kernel/reliefnt/fs"
LOG_DIR = ROOT / "build/logs/ext4-interop"
EXT4_SOURCES = [
    FS / f"storage_ext4_{name}.c"
    for name in ("format", "checksum", "cache", "alloc", "extent", "ops", "xattr", "journal")
]


def run_logged(argv: list[str], log: Path, *, check: bool = True) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(argv, cwd=ROOT, text=True, capture_output=True, check=False)
    with log.open("a", encoding="utf-8") as handle:
        handle.write("$ " + " ".join(map(str, argv)) + "\n")
        handle.write(result.stdout)
        handle.write(result.stderr)
    if check and result.returncode:
        raise subprocess.CalledProcessError(result.returncode, argv, result.stdout, result.stderr)
    return result


def inode_number(image: Path, path: str) -> str:
    output = run_checked(["debugfs", "-R", f"stat {path}", str(image)]).stdout
    match = re.search(r"Inode:\s+(\d+)", output)
    if not match:
        raise RuntimeError(f"debugfs did not return an inode for {path}: {output}")
    return match.group(1)


def build_native_reader(work: Path) -> Path:
    binary = work / "native-read"
    run_checked([
        "cc", "-std=c11", "-O1", "-g", "-D_GNU_SOURCE",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-DRELIEFOS_STORAGE_STANDALONE_TU", "-Ikernel/reliefnt/include", "-Iinclude",
        "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/include",
        "-include", str(STORAGE / "storage_internal.h"),
        "tools/tests/ext4_extent_test.c", *map(str, EXT4_SOURCES), "-o", str(binary),
    ])
    return binary


def build_native_writer(work: Path) -> Path:
    binary = work / "native-write"
    run_checked([
        "cc", "-std=c11", "-O1", "-g", "-D_GNU_SOURCE",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-DRELIEFOS_STORAGE_STANDALONE_TU", "-Ikernel/reliefnt/include", "-Iinclude",
        "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/include",
        "-include", str(STORAGE / "storage_internal.h"),
        "tools/tests/ext4_write_test.c", *map(str, EXT4_SOURCES), "-o", str(binary),
    ])
    return binary


def make_stage(stage: Path) -> None:
    (stage / "deep" / "one" / "two" / "three").mkdir(parents=True)
    (stage / "deep" / "one" / "two" / "three" / "payload").write_bytes(
        bytes((index * 17 + 3) % 251 for index in range(4 * 1024 * 1024))
    )
    (stage / "short").write_bytes(b"reliefos-ext4-interop\n")
    sparse = stage / "sparse"
    with sparse.open("wb") as handle:
        handle.write(b"head")
        handle.seek(8 * 1024 * 1024 - 4)
        handle.write(b"tail")
    for index in range(256):
        (stage / "small").mkdir(exist_ok=True)
        (stage / "small" / f"file-{index:04d}").write_bytes(f"small-{index:04d}\n".encode())
    (stage / "hardlink").hardlink_to(stage / "short")
    (stage / "symlink").symlink_to("short")


def compare_file(image: Path, image_path: str, expected: Path, work: Path) -> None:
    actual = work / (image_path.strip("/").replace("/", "_") + ".actual")
    run_checked(["debugfs", "-R", f"dump {image_path} {actual}", str(image)])
    if hashlib.sha256(actual.read_bytes()).hexdigest() != hashlib.sha256(expected.read_bytes()).hexdigest():
        raise AssertionError(f"content mismatch for {image_path}")


def linux_to_reliefos(work: Path, log: Path) -> None:
    stage = work / "linux-stage"
    stage.mkdir()
    make_stage(stage)
    image = work / "linux-to-reliefos.ext4"
    create_image(image, 128, ["filetype", "extents", "dir_index", "metadata_csum", "64bit",
                              "flex_bg", "has_journal", "large_file", "huge_file", "extra_isize"])
    # Populate through mke2fs, then let native ReliefOS code read the largest
    # extent-bearing file from that Linux image.
    run_logged(["debugfs", "-w", "-R", f"write {stage / 'short'} /short", str(image)], log)
    run_logged(["debugfs", "-w", "-R", f"mkdir /deep", str(image)], log)
    run_logged(["debugfs", "-w", "-R", f"write {stage / 'deep/one/two/three/payload'} /payload", str(image)], log)
    e2fsck_read_only(image)
    expected = stage / "deep/one/two/three/payload"
    native = build_native_reader(work)
    run_logged([str(native), str(image), inode_number(image, "/payload"), str(expected)], log)
    compare_file(image, "/payload", expected, work)
    compare_file(image, "/short", stage / "short", work)
    log.write_text(log.read_text(encoding="utf-8") + "PASS linux-to-reliefos native read and e2fsck\n", encoding="utf-8")


def reliefos_to_linux(work: Path, log: Path) -> None:
    stage = work / "reliefos-stage"
    stage.mkdir()
    (stage / "payload").touch()
    image = work / "reliefos-input.ext4"
    create_image(image, 128, ["filetype", "extents", "dir_index", "metadata_csum", "64bit",
                              "flex_bg", "has_journal", "large_file", "huge_file", "extra_isize"])
    run_logged(["debugfs", "-w", "-R", f"write {stage / 'payload'} /payload", str(image)], log)
    native = build_native_writer(work)
    expected = work / "native-expected"
    result = work / "reliefos-to-linux.ext4"
    run_logged([str(native), str(image), inode_number(image, "/payload"), str(result), str(expected)], log)
    e2fsck_read_only(result)
    actual = work / "reliefos-payload"
    run_checked(["debugfs", "-R", f"dump /payload {actual}", str(result)])
    if actual.read_bytes() != expected.read_bytes():
        raise AssertionError("native ReliefOS write did not survive the Linux image oracle")
    log.write_text(log.read_text(encoding="utf-8") + "PASS reliefos-to-linux native write and e2fsck\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--direction", choices=("linux-to-reliefos", "reliefos-to-linux"), required=True)
    parser.add_argument("--mode", choices=("fixture",), default="fixture")
    args = parser.parse_args()
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    log = LOG_DIR / f"{args.direction}.log"
    log.write_text("", encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="reliefos-ext4-interop-", dir=ROOT / "build") as directory:
        work = Path(directory)
        if args.direction == "linux-to-reliefos":
            linux_to_reliefos(work, log)
        else:
            reliefos_to_linux(work, log)
    print(f"PASS {args.direction} fixture; log={log}")


if __name__ == "__main__":
    main()
