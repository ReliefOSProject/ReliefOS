#!/usr/bin/env python3
"""Create a ReliefOS GPT disk with a FAT32 ESP and an ext4 runtime root.

The default root filesystem is ext4.  ext2 remains an explicit compatibility
option for older images.
"""
from __future__ import annotations

import argparse
import fcntl
import os
import re
import shutil
import struct
import sys
import subprocess
import tempfile
import uuid
import zlib
from contextlib import contextmanager
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from make_ext2_root import populate_ext2
from make_ext4_root import populate_ext4
from image_test_accounts import seed_test_accounts
from reliefos_layout import (  # noqa: E402  (tools directory is not a package)
    ETC_RELIEFOS,
    layout_directories,
    apply_root_symlinks,
    ESP_DISPLAY_CONF,
    ESP_DISPLAY_CONF_LEGACY,
    ESP_KERNEL,
    ESP_KERNEL_LEGACY,
)
SECTOR_SIZE = 512
ESP_FIRST_SECTOR = 2048
GPT_HEADER_SIZE = 92
GPT_ENTRY_COUNT = 128
GPT_ENTRY_SIZE = 128
GPT_ENTRY_TABLE_SIZE = GPT_ENTRY_COUNT * GPT_ENTRY_SIZE
GPT_ENTRY_TABLE_SECTORS = GPT_ENTRY_TABLE_SIZE // SECTOR_SIZE
GPT_PRIMARY_HEADER_LBA = 1
GPT_PRIMARY_ENTRIES_LBA = 2
EFI_SYSTEM_PARTITION_GUID = uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b")
LINUX_FILESYSTEM_GUID = uuid.UUID("0fc63daf-8483-4772-8e79-3d69d8477de4")
MICROSOFT_BASIC_DATA_GUID = uuid.UUID("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7")


def run(cmd: list[str]) -> None:
    print("+", " ".join(cmd))
    subprocess.run(cmd, cwd=ROOT, check=True)


def copy_file(src: Path, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def temporary_output(path: Path) -> Path:
    """Return a same-directory temporary path suitable for atomic replacement."""
    return path.with_name(f".{path.name}.{os.getpid()}.tmp")


@contextmanager
def image_lock(raw: Path):
    """Reject concurrent image writers before they can race on final outputs."""
    lock_path = raw.with_name(f".{raw.name}.lock")
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    handle = lock_path.open("a+", encoding="utf-8")
    try:
        try:
            fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise SystemExit(
                f"another VMDK build is already writing {raw}; wait for it to finish"
            ) from error
        yield
    finally:
        fcntl.flock(handle.fileno(), fcntl.LOCK_UN)
        handle.close()


def gpt_header(current_lba: int, backup_lba: int, first_usable_lba: int,
               last_usable_lba: int, disk_guid: uuid.UUID,
               entries_lba: int, entries_crc: int) -> bytes:
    """Build one CRC-protected GPT header for the fixed ReliefOS table layout."""
    header = bytearray(GPT_HEADER_SIZE)
    struct.pack_into(
        "<8sIIIIQQQQ16sQIII", header, 0,
        b"EFI PART", 0x00010000, GPT_HEADER_SIZE, 0, 0,
        current_lba, backup_lba, first_usable_lba, last_usable_lba,
        disk_guid.bytes_le, entries_lba, GPT_ENTRY_COUNT, GPT_ENTRY_SIZE,
        entries_crc,
    )
    struct.pack_into("<I", header, 16, zlib.crc32(header) & 0xFFFFFFFF)
    return bytes(header)


def write_gpt(image: Path, partitions: list[tuple[uuid.UUID, int, int, str]]) -> list[uuid.UUID]:
    """Write a standard primary and backup GPT without host partition tools."""
    image_size = image.stat().st_size
    total_sectors = image_size // SECTOR_SIZE
    if image_size % SECTOR_SIZE or total_sectors < 2 * GPT_ENTRY_TABLE_SECTORS + 3:
        raise ValueError("GPT image size must contain aligned primary and backup tables")
    if len(partitions) > GPT_ENTRY_COUNT:
        raise ValueError("too many GPT partitions")

    first_usable_lba = GPT_PRIMARY_ENTRIES_LBA + GPT_ENTRY_TABLE_SECTORS
    backup_header_lba = total_sectors - 1
    backup_entries_lba = backup_header_lba - GPT_ENTRY_TABLE_SECTORS
    last_usable_lba = backup_entries_lba - 1
    entries = bytearray(GPT_ENTRY_TABLE_SIZE)
    partition_uuids = [uuid.uuid4() for _ in partitions]
    for index, (type_guid, first_lba, last_lba, name) in enumerate(partitions):
        if first_lba < first_usable_lba or last_lba > last_usable_lba or first_lba > last_lba:
            raise ValueError(f"GPT partition {index + 1} is outside the usable LBA range")
        encoded_name = name.encode("utf-16le")
        if len(encoded_name) > 72:
            raise ValueError(f"GPT partition {index + 1} name is too long")
        struct.pack_into(
            "<16s16sQQQ72s", entries, index * GPT_ENTRY_SIZE,
            type_guid.bytes_le, partition_uuids[index].bytes_le, first_lba, last_lba,
            0, encoded_name.ljust(72, b"\0"),
        )

    entries_crc = zlib.crc32(entries) & 0xFFFFFFFF
    disk_guid = uuid.uuid4()
    primary_header = gpt_header(
        GPT_PRIMARY_HEADER_LBA, backup_header_lba, first_usable_lba,
        last_usable_lba, disk_guid, GPT_PRIMARY_ENTRIES_LBA, entries_crc,
    )
    backup_header = gpt_header(
        backup_header_lba, GPT_PRIMARY_HEADER_LBA, first_usable_lba,
        last_usable_lba, disk_guid, backup_entries_lba, entries_crc,
    )
    protective_mbr = bytearray(SECTOR_SIZE)
    struct.pack_into(
        "<B3sB3sII", protective_mbr, 446, 0,
        b"\0\x02\0", 0xEE, b"\xff\xff\xff", 1,
        min(total_sectors - 1, 0xFFFFFFFF),
    )
    protective_mbr[510:512] = b"\x55\xaa"

    with image.open("r+b") as stream:
        stream.seek(0)
        stream.write(protective_mbr)
        stream.seek(GPT_PRIMARY_HEADER_LBA * SECTOR_SIZE)
        stream.write(primary_header)
        stream.seek(GPT_PRIMARY_ENTRIES_LBA * SECTOR_SIZE)
        stream.write(entries)
        stream.seek(backup_entries_lba * SECTOR_SIZE)
        stream.write(entries)
        stream.seek(backup_header_lba * SECTOR_SIZE)
        stream.write(backup_header)
    return partition_uuids


def write_root_fstab(root: Path, root_uuid: uuid.UUID, esp_uuid: uuid.UUID,
                     root_fs: str = "ext4") -> None:
    """Describe this disk's actual GPT extents; never reuse host device numbering."""
    fstab = root / "etc/fstab"
    if fstab.is_symlink():
        raise ValueError("root fstab must be a regular configuration file")
    fstab.write_text(
        "# <source> <mountpoint> <type> <options> <dump> <pass>\n"
        f"/dev/disk/by-partuuid/{root_uuid} / {root_fs} defaults 0 1\n"
        f"/dev/disk/by-partuuid/{esp_uuid} /boot vfat defaults 0 2\n",
        encoding="ascii",
    )
    fstab.chmod(0o644)


def make_boot_tree(staging: Path, destination: Path) -> None:
    """Stage only files GRUB and the kernel need before the root mounts.

    The canonical ESP namespace is /reliefos. Byte-identical /leonos payloads
    are also staged for the legacy rollback GRUB entry.
    """
    copy_file(staging / "EFI/BOOT/BOOTX64.EFI", destination / "EFI/BOOT/BOOTX64.EFI")
    shutil.copytree(staging / "grub", destination / "grub", symlinks=True,
                    dirs_exist_ok=True)
    copy_file(staging / ESP_KERNEL.lstrip("/"), destination / ESP_KERNEL.lstrip("/"))
    copy_file(staging / ESP_KERNEL_LEGACY.lstrip("/"),
              destination / ESP_KERNEL_LEGACY.lstrip("/"))
    # The kernel reads the boot theme from the ESP copy before any root
    # filesystem exists, so the generated display.conf is duplicated at both
    # ESP paths. It comes from the same source as /etc/reliefos/display.conf.
    display = staging / ETC_RELIEFOS / "display.conf"
    if display.is_file():
        copy_file(display, destination / ESP_DISPLAY_CONF.lstrip("/"))
        copy_file(display, destination / ESP_DISPLAY_CONF_LEGACY.lstrip("/"))


def make_root_tree(staging: Path, destination: Path, language: str) -> None:
    """Stage the normal writable root without duplicating ESP-only boot files."""
    if not re.fullmatch(r"[A-Za-z0-9._@-]+", language):
        raise ValueError("invalid locale name")
    # symlinks=True is required: /var/run, /bin/sh and command entries are
    # real relative symlinks and must not be dereferenced into copies.
    shutil.copytree(staging, destination, symlinks=True, dirs_exist_ok=True)
    shutil.rmtree(destination / "EFI", ignore_errors=True)
    shutil.rmtree(destination / "grub", ignore_errors=True)
    shutil.rmtree(destination / "reliefos", ignore_errors=True)
    shutil.rmtree(destination / "leonos", ignore_errors=True)
    layout_directories(destination)
    apply_root_symlinks(destination)
    locale = destination / ETC_RELIEFOS / "locale.conf"
    locale.parent.mkdir(parents=True, exist_ok=True)
    locale.write_text(f"LANG={language}\n", encoding="utf-8")
    seed_test_accounts(destination)


def main() -> int:
    parser = argparse.ArgumentParser(description="Create ReliefOS GPT FAT32-ESP/ext4-root VMDK")
    parser.add_argument("--out", default="build/images/reliefos.vmdk")
    parser.add_argument("--raw", default="build/images/reliefos.raw")
    parser.add_argument("--esp-tree", default="build/esp")
    parser.add_argument("--root-image")
    parser.add_argument("--root-fs", choices=("ext4", "ext2"), default="ext4",
                        help="Runtime root filesystem (ext4 by default; ext2 compatibility)")
    parser.add_argument("--esp-image", default="build/images/esp.fat")
    parser.add_argument("--default-lang", default="en_US.UTF-8",
                        help="Locale name written into this VMDK root filesystem")
    parser.add_argument("--size-mib", type=int, default=512)
    parser.add_argument("--esp-size-mib", type=int, default=128)
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9._@-]+", args.default_lang):
        parser.error("--default-lang requires a locale name")

    raw = ROOT / args.raw
    out = ROOT / args.out
    esp_tree = ROOT / args.esp_tree
    root_image = ROOT / (args.root_image or f"build/images/root.{args.root_fs}")
    esp_image = ROOT / args.esp_image
    if not esp_tree.is_dir():
        raise SystemExit(f"ESP staging tree does not exist: {esp_tree}")
    if args.esp_size_mib < 128 or args.size_mib <= args.esp_size_mib + 128:
        raise SystemExit("VMDK needs a 128 MiB+ FAT32 ESP and at least 128 MiB runtime root space")

    for path in (raw, out, root_image, esp_image):
        path.parent.mkdir(parents=True, exist_ok=True)
    raw_temp = temporary_output(raw)
    out_temp = temporary_output(out)
    root_temp = temporary_output(root_image)
    esp_temp = temporary_output(esp_image)
    temporary_files = (raw_temp, out_temp, root_temp, esp_temp)

    with image_lock(raw):
        try:
            for path in temporary_files:
                path.unlink(missing_ok=True)

            run(["truncate", "-s", f"{args.size_mib}M", str(raw_temp)])
            total_sectors = raw_temp.stat().st_size // SECTOR_SIZE
            esp_sectors = args.esp_size_mib * 1024 * 1024 // SECTOR_SIZE
            esp_last = ESP_FIRST_SECTOR + esp_sectors - 1
            root_first = (esp_last + 1 + 2047) & ~2047
            root_last = total_sectors - 2048
            if root_last <= root_first or root_last - root_first + 1 < 262144:
                raise SystemExit("VMDK root partition is smaller than the 128 MiB minimum")
            partition_uuids = write_gpt(raw_temp, [
                (EFI_SYSTEM_PARTITION_GUID, ESP_FIRST_SECTOR, esp_last, "RELIEFOS_ESP"),
                (LINUX_FILESYSTEM_GUID,
                 root_first, root_last, "RELIEFOS_ROOT"),
            ])

            with tempfile.TemporaryDirectory(prefix="reliefos-vmdk-") as temp_dir:
                temp = Path(temp_dir)
                boot_tree = temp / "esp"
                root_tree = temp / "root"
                make_boot_tree(esp_tree, boot_tree)
                make_root_tree(esp_tree, root_tree, args.default_lang)
                write_root_fstab(root_tree, partition_uuids[1], partition_uuids[0], args.root_fs)

                run(["truncate", "-s", str(esp_sectors * SECTOR_SIZE), str(esp_temp)])
                run(["mkfs.fat", "-F", "32", "-s", "2", "-n", "RELIEFOS", str(esp_temp)])
                for item in sorted(boot_tree.iterdir()):
                    run(["mcopy", "-s", "-i", str(esp_temp), str(item), "::/"])

                root_bytes = (root_last - root_first + 1) * SECTOR_SIZE
                run(["truncate", "-s", str(root_bytes), str(root_temp)])
                inode_count = max(8192, sum(1 for _ in root_tree.rglob("*")) * 2)
                if args.root_fs == "ext4":
                    populate_ext4(root_tree, root_temp, inode_count,
                                  str(partition_uuids[1]))
                else:
                    populate_ext2(root_tree, root_temp, inode_count)
                run(["e2fsck", "-f", "-n", str(root_temp)])

            run(["dd", f"if={esp_temp}", f"of={raw_temp}", "bs=512",
                 f"seek={ESP_FIRST_SECTOR}", "conv=notrunc", "status=none"])
            run(["dd", f"if={root_temp}", f"of={raw_temp}", "bs=512",
                 f"seek={root_first}", "conv=notrunc", "status=none"])
            run(["qemu-img", "convert", "-f", "raw", "-O", "vmdk", str(raw_temp), str(out_temp)])
            for source, destination in ((esp_temp, esp_image), (root_temp, root_image),
                                        (raw_temp, raw), (out_temp, out)):
                os.replace(source, destination)
        finally:
            for path in temporary_files:
                path.unlink(missing_ok=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
