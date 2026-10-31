#!/usr/bin/env python3
"""Unit tests for the host-independent ReliefOS GPT image writer."""

from __future__ import annotations

import struct
import shutil
import subprocess
import tempfile
import unittest
import uuid
import zlib
from pathlib import Path

from make_image import (
    EFI_SYSTEM_PARTITION_GUID,
    GPT_ENTRY_TABLE_SIZE,
    GPT_PRIMARY_ENTRIES_LBA,
    GPT_PRIMARY_HEADER_LBA,
    LINUX_FILESYSTEM_GUID,
    MICROSOFT_BASIC_DATA_GUID,
    SECTOR_SIZE,
    write_gpt,
    make_root_tree,
    make_boot_tree,
)
from populate_exfat import ExfatVolume


HEADER_FORMAT = "<8sIIIIQQQQ16sQIII"


def read_header(image: bytes, lba: int) -> tuple[object, ...]:
    return struct.unpack_from(HEADER_FORMAT, image, lba * SECTOR_SIZE)


def assert_header_crc(test: unittest.TestCase, image: bytes, lba: int) -> None:
    header = bytearray(image[lba * SECTOR_SIZE:lba * SECTOR_SIZE + 92])
    stored_crc = struct.unpack_from("<I", header, 16)[0]
    struct.pack_into("<I", header, 16, 0)
    test.assertEqual(stored_crc, zlib.crc32(header) & 0xFFFFFFFF)


class LocaleSeedTests(unittest.TestCase):
    def test_boot_tree_stages_canonical_and_legacy_esp_payloads(self) -> None:
        with tempfile.TemporaryDirectory(prefix="reliefos-esp-layout-") as directory:
            root = Path(directory)
            staging = root / "staging"
            destination = root / "esp"
            (staging / "EFI/BOOT").mkdir(parents=True)
            (staging / "grub").mkdir()
            for name, value in (
                ("EFI/BOOT/BOOTX64.EFI", b"efi"),
                ("grub/grub.cfg", b"menu"),
                ("reliefos/kernel.sys", b"kernel"),
                ("leonos/kernel.sys", b"kernel"),
                ("etc/reliefos/display.conf", b"theme=metro\n"),
            ):
                path = staging / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(value)

            make_boot_tree(staging, destination)
            self.assertEqual((destination / "reliefos/kernel.sys").read_bytes(), b"kernel")
            self.assertEqual((destination / "leonos/kernel.sys").read_bytes(), b"kernel")
            self.assertEqual((destination / "reliefos/config/display.conf").read_bytes(),
                             b"theme=metro\n")
            self.assertEqual((destination / "leonos/config/display.conf").read_bytes(),
                             b"theme=metro\n")

    def test_root_locale_seed(self) -> None:
        with tempfile.TemporaryDirectory(prefix="reliefos-locale-test-") as directory:
            root = Path(directory)
            staging = root / "staging"
            (staging / "etc/skel").mkdir(parents=True)
            (staging / "etc/sudoers").write_text("%wheel ALL=(ALL:ALL) ALL\n")
            for locale in ("zh_CN.UTF-8", "en_US.UTF-8"):
                destination = root / locale
                make_root_tree(staging, destination, locale)
                self.assertEqual((destination / "etc/reliefos/locale.conf").read_text(),
                                 f"LANG={locale}\n")
            with self.assertRaises(ValueError):
                make_root_tree(staging, root / "invalid", "../bad")
            self.assertFalse((root / "invalid").exists())


class GptWriterTests(unittest.TestCase):
    def test_writes_valid_primary_and_backup_gpt(self) -> None:
        with tempfile.TemporaryDirectory(prefix="reliefos-gpt-test-") as directory:
            image_path = Path(directory) / "disk.raw"
            image_size = 64 * 1024 * 1024
            with image_path.open("wb") as image:
                image.truncate(image_size)
            total_sectors = image_size // SECTOR_SIZE
            esp_last = 34815
            root_first = 34816
            root_last = total_sectors - 2048
            write_gpt(image_path, [
                (EFI_SYSTEM_PARTITION_GUID, 2048, esp_last, "LEONOS4_ESP"),
                (MICROSOFT_BASIC_DATA_GUID, root_first, root_last, "LEONOS4_ROOT"),
            ])

            image = image_path.read_bytes()
            self.assertEqual(image[510:512], b"\x55\xaa")
            self.assertEqual(image[450], 0xEE)

            primary = read_header(image, GPT_PRIMARY_HEADER_LBA)
            self.assertEqual(primary[0], b"EFI PART")
            self.assertEqual(primary[1], 0x00010000)
            self.assertEqual(primary[2], 92)
            self.assertEqual(primary[5], GPT_PRIMARY_HEADER_LBA)
            self.assertEqual(primary[6], total_sectors - 1)
            self.assertEqual(primary[10], GPT_PRIMARY_ENTRIES_LBA)
            self.assertEqual(primary[11:13], (128, 128))
            assert_header_crc(self, image, GPT_PRIMARY_HEADER_LBA)

            entries_offset = primary[10] * SECTOR_SIZE
            entries = image[entries_offset:entries_offset + GPT_ENTRY_TABLE_SIZE]
            self.assertEqual(primary[13], zlib.crc32(entries) & 0xFFFFFFFF)
            first = struct.unpack_from("<16s16sQQQ72s", entries, 0)
            second = struct.unpack_from("<16s16sQQQ72s", entries, 128)
            self.assertEqual(first[0], EFI_SYSTEM_PARTITION_GUID.bytes_le)
            self.assertNotEqual(first[1], uuid.UUID(int=0).bytes_le)
            self.assertEqual(first[2:4], (2048, esp_last))
            self.assertEqual(first[5].decode("utf-16le").rstrip("\0"), "LEONOS4_ESP")
            self.assertEqual(second[0], MICROSOFT_BASIC_DATA_GUID.bytes_le)
            self.assertEqual(second[2:4], (root_first, root_last))
            self.assertEqual(second[5].decode("utf-16le").rstrip("\0"), "LEONOS4_ROOT")

            backup_lba = total_sectors - 1
            backup = read_header(image, backup_lba)
            self.assertEqual(backup[0], b"EFI PART")
            self.assertEqual(backup[5:7], (backup_lba, GPT_PRIMARY_HEADER_LBA))
            self.assertEqual(backup[9], primary[9])
            self.assertEqual(backup[13], primary[13])
            backup_entries = image[backup[10] * SECTOR_SIZE:backup[10] * SECTOR_SIZE + GPT_ENTRY_TABLE_SIZE]
            self.assertEqual(backup_entries, entries)
            assert_header_crc(self, image, backup_lba)

    def test_ext2_compatibility_gpt_type_remains_available(self) -> None:
        with tempfile.TemporaryDirectory(prefix="reliefos-gpt-ext2-test-") as directory:
            image_path = Path(directory) / "disk.raw"
            with image_path.open("wb") as image:
                image.truncate(64 * 1024 * 1024)
            write_gpt(image_path, [
                (LINUX_FILESYSTEM_GUID, 2048, 32767, "LEGACY_EXT2"),
            ])
            entry = image_path.read_bytes()[GPT_PRIMARY_ENTRIES_LBA * SECTOR_SIZE:
                                            GPT_PRIMARY_ENTRIES_LBA * SECTOR_SIZE + 128]
            self.assertEqual(struct.unpack_from("<16s", entry)[0], LINUX_FILESYSTEM_GUID.bytes_le)


@unittest.skipUnless(shutil.which("mkfs.exfat") and shutil.which("fsck.exfat"),
                     "exfatprogs is required for exFAT image validation")
class ExfatPopulationTests(unittest.TestCase):
    def test_standard_volume_metadata_and_expanded_directory(self) -> None:
        with tempfile.TemporaryDirectory(prefix="reliefos-exfat-test-") as directory:
            root = Path(directory)
            image_path = root / "root.exfat"
            staging = root / "staging"
            data = staging / "data"
            data.mkdir(parents=True)
            (staging / "Unicode-世界.txt").write_text("ReliefOS exFAT\n", encoding="utf-8")
            # More than one 4 KiB directory cluster of 3-entry file sets.
            for index in range(96):
                (data / f"entry-{index:03d}.txt").write_text(str(index), encoding="ascii")

            with image_path.open("wb") as image:
                image.truncate(32 * 1024 * 1024)
            subprocess.run(["mkfs.exfat", "-q", "-L", "LEONOS4ROOT", str(image_path)],
                           check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            volume = ExfatVolume(image_path)
            try:
                volume.populate(staging)
            finally:
                volume.close()

            check = subprocess.run(["fsck.exfat", "-n", str(image_path)], check=False,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            self.assertEqual(check.returncode, 0, check.stdout + check.stderr)
            volume = ExfatVolume(image_path)
            try:
                self.assertTrue(volume.bitmap_cluster >= 2)
                self.assertTrue(volume.upcase_cluster >= 2)
                self.assertGreater(volume.upcase_length, 4096)
                unicode_entry = volume.find_in_dir(volume.root_cluster, False, "unicode-世界.txt")
                self.assertIsNotNone(unicode_entry)
                data_entry = volume.find_in_dir(volume.root_cluster, False, "DATA")
                self.assertIsNotNone(data_entry)
                assert data_entry is not None
                self.assertFalse(data_entry[1]["nofat"])
                self.assertGreater(data_entry[1]["size"], volume.cluster_bytes)
                nested = volume.find_in_dir(data_entry[1]["first"], False, "ENTRY-095.TXT")
                self.assertIsNotNone(nested)
            finally:
                volume.close()


if __name__ == "__main__":
    unittest.main()
