#!/usr/bin/env python3
"""Host driver for the ext4 mount, feature-policy and cache tests.

Builds tools/tests/ext4_mount_test.c (twice: default cache capacities and a
2-entry build that proves the EXT4_*_CACHE_ENTRIES macros are overridable),
generates real mke2fs images through tools/tests/ext4_fixture.py, applies
byte-level patches for the corrupt/unsupported cases, and runs one C case per
matrix entry.  The C fixture asserts storage_ext4_mount() return codes and
volume fields; this driver owns image construction and result aggregation.

Run: python3 tools/tests/ext4_mount_test.py
"""
from __future__ import annotations

import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from ext4_fixture import create_image, e2fsck_read_only, run_checked  # noqa: E402

FEATURES_FULL = ["filetype", "extents", "64bit", "flex_bg", "metadata_csum",
                 "extra_isize", "dir_nlink", "huge_file"]
FEATURES_PLAIN = ["filetype", "extents"]
FEATURES_EXT2 = ["filetype", "sparse_super", "large_file"]
FEATURES_JOURNAL = ["filetype", "has_journal"]

# ext4_super_block / ext4_group_desc / ext4_inode field offsets
# (linux/fs/ext4/ext4.h, Linux v7.3-rc5 reference tree).
SB_OFFSET = 1024
SB_BLOCKS_COUNT_LO = 0x04
SB_MAGIC = 0x38
SB_FEATURE_COMPAT = 0x5C
SB_FEATURE_INCOMPAT = 0x60
SB_FEATURE_RO_COMPAT = 0x64
SB_VOLUME_NAME = 0x78
SB_DESC_SIZE = 0xFE
GD_CHECKSUM = 0x1E
GD_INODE_TABLE_LO = 0x08
INO_CSUM_LO = 0x7C

CACHE_MACRO_FLAGS = [
    "-DEXT4_BLOCK_CACHE_ENTRIES=2",
    "-DEXT4_INODE_CACHE_ENTRIES=2",
    "-DEXT4_DIR_CACHE_ENTRIES=2",
    "-DEXT4_JOURNAL_CACHE_ENTRIES=2",
]

RUN_ENV = {
    "ASAN_OPTIONS": "detect_leaks=1",
    "UBSAN_OPTIONS": "halt_on_error=1",
}


def compile_test(work: Path, name: str, extra_flags: tuple[str, ...] = ()) -> Path:
    binary = work / name
    run_checked([
        "cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        *extra_flags,
        "-Ikernel/reliefnt/include", "-Iinclude",
        "-Ikernel/reliefnt/include/uapi",
        "-Ikernel/reliefnt/include",
        "tools/tests/ext4_mount_test.c", "-o", str(binary),
    ], cwd=ROOT)
    return binary


def clone(work: Path, base: Path, name: str) -> Path:
    target = work / name
    shutil.copyfile(base, target)
    return target


def patch_u16(path: Path, offset: int, value: int) -> None:
    with path.open("r+b") as handle:
        handle.seek(offset)
        handle.write(struct.pack("<H", value))


def patch_u32(path: Path, offset: int, value: int) -> None:
    with path.open("r+b") as handle:
        handle.seek(offset)
        handle.write(struct.pack("<I", value))


def patch_or_u32(path: Path, offset: int, bits: int) -> None:
    with path.open("r+b") as handle:
        handle.seek(offset)
        value = struct.unpack("<I", handle.read(4))[0]
        handle.seek(offset)
        handle.write(struct.pack("<I", value | bits))


def patch_flip_byte(path: Path, offset: int) -> None:
    with path.open("r+b") as handle:
        handle.seek(offset)
        byte = handle.read(1)[0]
        handle.seek(offset)
        handle.write(bytes([byte ^ 0xFF]))


def read_u16(path: Path, offset: int) -> int:
    with path.open("rb") as handle:
        handle.seek(offset)
        return struct.unpack("<H", handle.read(2))[0]


def read_u32(path: Path, offset: int) -> int:
    with path.open("rb") as handle:
        handle.seek(offset)
        return struct.unpack("<I", handle.read(4))[0]


def gd_offset(image: Path, group: int, block_size: int) -> int:
    return block_size + group * read_u16(image, SB_OFFSET + SB_DESC_SIZE)


def build_images(work: Path) -> dict[str, Path]:
    full = work / "full.ext4"
    create_image(full, 16, FEATURES_FULL)
    e2fsck_read_only(full)
    plain = work / "plain.ext4"
    create_image(plain, 16, FEATURES_PLAIN)
    e2fsck_read_only(plain)
    ext2 = work / "classic.ext2"
    create_image(ext2, 16, FEATURES_EXT2)
    e2fsck_read_only(ext2)
    journal = work / "journal.ext3"
    create_image(journal, 16, FEATURES_JOURNAL)
    e2fsck_read_only(journal)
    multi = work / "multi.ext4"
    with multi.open("wb") as handle:
        handle.truncate(16 * 1024 * 1024)
    run_checked([
        "mke2fs", "-t", "ext4", "-F", "-b", "4096", "-I", "256",
        "-O", "none," + ",".join(FEATURES_FULL), "-m", "0", "-g", "1024",
        str(multi),
    ])
    e2fsck_read_only(multi)

    images: dict[str, Path] = {
        "valid": full,
        "valid-partition-offset": work / "offset.ext4",
        "ext2-classify": ext2,
        "multi-group": multi,
        "cache-alloc-fail": full,
        "cache": full,
        "cache-small": full,
        "sync": full,
    }
    # Partition-offset case: one MiB of padding in front of the filesystem.
    offset = images["valid-partition-offset"]
    with offset.open("wb") as out, full.open("rb") as src:
        out.write(b"\0" * (1024 * 1024))
        shutil.copyfileobj(src, out)

    # Failure cases: patched plain images (no metadata checksums, so a
    # feature/count patch is itself the fault under test).
    bad_magic = clone(work, plain, "bad-magic.ext4")
    patch_u16(bad_magic, SB_OFFSET + SB_MAGIC, 0)
    images["bad-magic"] = bad_magic

    huge = clone(work, plain, "huge-blocks.ext4")
    patch_u32(huge, SB_OFFSET + SB_BLOCKS_COUNT_LO, 0xFFFFFFF0)
    images["huge-blocks-count"] = huge

    images["boundary-small-partition"] = full

    super_csum = clone(work, full, "super-csum.ext4")
    patch_flip_byte(super_csum, SB_OFFSET + SB_VOLUME_NAME)
    images["super-checksum-broken"] = super_csum

    gd_csum = clone(work, full, "gd-csum.ext4")
    patch_flip_byte(gd_csum, gd_offset(full, 0, 4096) + GD_CHECKSUM)
    images["gd-checksum-broken"] = gd_csum
    images["route-corrupt-metadata"] = gd_csum

    gd_group3 = clone(work, multi, "gd-group3.ext4")
    patch_flip_byte(gd_group3, gd_offset(multi, 3, 4096) + GD_CHECKSUM)
    images["gd-checksum-group3"] = gd_group3

    inode_csum = clone(work, full, "ino-csum.ext4")
    inode_table = read_u32(full, gd_offset(full, 0, 4096) + GD_INODE_TABLE_LO)
    patch_flip_byte(inode_csum, inode_table * 4096 + 256 + INO_CSUM_LO)
    images["inode-checksum-broken"] = inode_csum

    # Feature-policy cases.
    def feature_case(name: str, base: Path, offset: int, bits: int) -> Path:
        target = clone(work, base, name + ".ext4")
        patch_or_u32(target, SB_OFFSET + offset, bits)
        images[name] = target
        return target

    feature_case("reject-unknown-incompat", plain, SB_FEATURE_INCOMPAT, 0x400000)
    feature_case("reject-incompat-compression", plain, SB_FEATURE_INCOMPAT, 0x1)
    feature_case("reject-incompat-journal-dev", plain, SB_FEATURE_INCOMPAT, 0x8)
    feature_case("reject-incompat-metabg", plain, SB_FEATURE_INCOMPAT, 0x10)
    feature_case("reject-incompat-mmp", plain, SB_FEATURE_INCOMPAT, 0x100)
    feature_case("reject-incompat-ea-inode", plain, SB_FEATURE_INCOMPAT, 0x400)
    feature_case("reject-incompat-dirdata", plain, SB_FEATURE_INCOMPAT, 0x1000)
    feature_case("reject-incompat-largedir", plain, SB_FEATURE_INCOMPAT, 0x4000)
    feature_case("reject-incompat-inline-data", plain, SB_FEATURE_INCOMPAT, 0x8000)
    feature_case("reject-incompat-encrypt", plain, SB_FEATURE_INCOMPAT, 0x10000)
    feature_case("reject-incompat-casefold", plain, SB_FEATURE_INCOMPAT, 0x20000)
    feature_case("reject-ro-quota", plain, SB_FEATURE_RO_COMPAT, 0x100)
    feature_case("reject-ro-bigalloc", plain, SB_FEATURE_RO_COMPAT, 0x200)
    feature_case("reject-ro-project", plain, SB_FEATURE_RO_COMPAT, 0x2000)
    feature_case("reject-ro-verity", plain, SB_FEATURE_RO_COMPAT, 0x8000)
    feature_case("reject-compat-fast-commit", plain, SB_FEATURE_COMPAT, 0x400)

    feature_case("ro-readonly-feature", plain, SB_FEATURE_RO_COMPAT, 0x1000)
    feature_case("ro-unknown-rocompat", plain, SB_FEATURE_RO_COMPAT, 0x40000000)
    feature_case("ro-unknown-rocompat-ext2shape", ext2, SB_FEATURE_RO_COMPAT, 0x40000000)
    feature_case("journal-recover", journal, SB_FEATURE_INCOMPAT, 0x4)
    feature_case("bare-recover", ext2, SB_FEATURE_INCOMPAT, 0x4)
    images["journal-clean"] = journal
    bad_journal = clone(work, journal, "journal-corrupt.img")
    journal_blocks = run_checked(["debugfs", "-R", "blocks <8>", str(journal)]).stdout.split()
    patch_flip_byte(bad_journal, int(journal_blocks[0]) * 4096)
    images["journal-corrupt"] = bad_journal

    # Root-route cases (storage_mount_ext_family): a policy-rejected image
    # must fail without the legacy ext2 fallback, while unrecognized images
    # and pure ext2 classifications keep it.
    feature_case("route-policy-reject", ext2, SB_FEATURE_RO_COMPAT, 0x200)
    images["route-probe-fallback"] = bad_magic
    images["route-ext2-classify"] = ext2
    images["route-ext4"] = full
    return images


CASE_ORDER = [
    "valid",
    "valid-partition-offset",
    "ext2-classify",
    "multi-group",
    "bad-magic",
    "huge-blocks-count",
    "boundary-small-partition",
    "super-checksum-broken",
    "gd-checksum-broken",
    "gd-checksum-group3",
    "inode-checksum-broken",
    "reject-unknown-incompat",
    "reject-incompat-compression",
    "reject-incompat-journal-dev",
    "reject-incompat-metabg",
    "reject-incompat-mmp",
    "reject-incompat-ea-inode",
    "reject-incompat-dirdata",
    "reject-incompat-largedir",
    "reject-incompat-inline-data",
    "reject-incompat-encrypt",
    "reject-incompat-casefold",
    "reject-ro-quota",
    "reject-ro-bigalloc",
    "reject-ro-project",
    "reject-ro-verity",
    "reject-compat-fast-commit",
    "ro-readonly-feature",
    "ro-unknown-rocompat",
    "ro-unknown-rocompat-ext2shape",
    "journal-recover",
    "journal-clean",
    "journal-corrupt",
    "bare-recover",
    "route-policy-reject",
    "route-corrupt-metadata",
    "route-probe-fallback",
    "route-ext2-classify",
    "route-ext4",
    "cache-alloc-fail",
    "cache",
    "sync",
]


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="ext4-mount-", dir=ROOT / "build") as directory:
        work = Path(directory)
        binary = compile_test(work, "ext4-mount-test")
        small = compile_test(work, "ext4-mount-test-small", tuple(CACHE_MACRO_FLAGS))
        images = build_images(work)
        for case in CASE_ORDER:
            subprocess.run([str(binary), str(images[case]), case], cwd=ROOT,
                           check=True, timeout=120, env={**RUN_ENV})
        subprocess.run([str(small), str(images["cache-small"]), "cache-small"], cwd=ROOT,
                       check=True, timeout=120, env={**RUN_ENV})
    print(f"PASS ext4 mount: {len(CASE_ORDER)} matrix cases plus cache-small on 2-entry caches")
    return 0


if __name__ == "__main__":
    sys.exit(main())
