#!/usr/bin/env python3
"""Check the actual bounded GRUB environment-record codec and pair selection."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parent / "tests"))
from kernel_slot_oracle import encode, decode, checksum, select, write_version


class KernelSlots(unittest.TestCase):
    def test_upstream_grub_reads_exact_whitelisted_environment(self):
        tool = os.environ.get("GRUB_EDITENV") or shutil.which("grub-editenv")
        bundled = Path(__file__).resolve().parents[1] / "out/kbuild-os-native/grub-host/bin/grub-editenv"
        if not tool and bundled.is_file():
            tool = str(bundled)
        if not tool:
            self.skipTest("build native GRUB tools or set GRUB_EDITENV for upstream format validation")
        fields = dict(generation=1, release="5.1.2-ko1-123456789abc", module_abi="123456789abc" + "0" * 52,
                      kernel_sha256="a" * 64, initramfs_sha256="b" * 64)
        with tempfile.TemporaryDirectory(prefix="reliefos-grub-slot-") as directory:
            path = Path(directory) / "kernel-A.env"
            path.write_bytes(encode(fields))
            result = subprocess.check_output([tool, str(path), "list"], text=True)
            self.assertEqual(dict(line.split("=", 1) for line in result.splitlines()),
                             {"reliefos_" + key: str(value) for key, value in fields.items()})

    def test_record_is_bounded_grub_environment(self):
        fields = dict(generation=1, release="5.1.2-ko1-123456789abc", module_abi="123456789abc" + "0" * 52,
                      kernel_sha256="a" * 64, initramfs_sha256="b" * 64)
        record = encode(fields)
        self.assertEqual(len(record), 1024)
        self.assertTrue(record.startswith(b"# GRUB Environment Block\n"))
        self.assertEqual(decode(record), fields)
        for field, value in (("release", "../unsafe"), ("generation", 0), ("generation", 10**20),
                             ("module_abi", "f" * 64), ("kernel_sha256", "Z" * 64)):
            with self.subTest(field=field), self.assertRaises(ValueError):
                encode({**fields, field: value})
        for record in (record[:-1], record + b"x", record.replace(b"reliefos_generation", b"dangerous_var______")):
            with self.assertRaises(ValueError):
                decode(record)

    def test_torn_inactive_slot_and_corrupt_pairs_fall_back(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-kernel-slots-") as directory:
            root = Path(directory)
            slots = root / "reliefos/state"
            slots.mkdir(parents=True)
            def publish(slot, generation):
                release = f"5.1.2-ko1-{generation:012x}"
                kernel, archive = b"kernel" + bytes([generation]), b"newc" + bytes([generation])
                fields = dict(generation=generation, release=release, module_abi=f"{generation:012x}" + "0" * 52,
                              kernel_sha256=hashlib.sha256(kernel).hexdigest(),
                              initramfs_sha256=hashlib.sha256(archive).hexdigest())
                write_version(root, fields, kernel, archive)
                record = encode(fields)
                (slots / f"kernel-{slot}.env").write_bytes(record)
                (slots / f"kernel-{slot}.sha256").write_bytes(checksum(slot, record))
                return fields, record
            old, old_record = publish("A", 1)
            self.assertEqual(select(root)["release"], old["release"])
            new, new_record = publish("B", 2)
            self.assertEqual(select(root)["release"], new["release"])
            for offset in (0, 24, 64, 128, 511, 1023):
                torn = bytearray(new_record)
                torn[offset] ^= 1
                (slots / "kernel-B.env").write_bytes(torn)
                self.assertEqual(select(root)["release"], old["release"])
            (slots / "kernel-B.env").write_bytes(new_record)
            original = (slots / "kernel-B.sha256").read_bytes()
            for length in (0, 1, 32, len(original)-1):
                (slots / "kernel-B.sha256").write_bytes(original[:length])
                self.assertEqual(select(root)["release"], old["release"])
            (slots / "kernel-B.sha256").write_bytes(original)
            (root / "reliefos/versions" / new["release"] / new["initramfs_sha256"] / "initramfs.cpio").write_bytes(b"corrupt")
            self.assertEqual(select(root)["release"], old["release"])
            (slots / "kernel-A.env").write_bytes(old_record[:128])
            with self.assertRaises(ValueError):
                select(root)


if __name__ == "__main__":
    unittest.main()
