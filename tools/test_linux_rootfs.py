#!/usr/bin/env python3
"""Verify image fstab generation against the installer GPT reader."""
from pathlib import Path
import subprocess
import tempfile
import unittest
import uuid
import make_image

ROOT = Path(__file__).resolve().parents[1]

class RootfsInterfaces(unittest.TestCase):
    def test_fstab_matches_installer_gpt_reader(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-fstab-") as directory:
            root = Path(directory)
            (root / "etc").mkdir()
            image = root / "disk.img"
            with image.open("wb") as stream:
                stream.truncate(16 * 1024 * 1024)
            partitions = make_image.write_gpt(image, [
                (uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b"), 2048, 4095, "ESP"),
                (uuid.UUID("0fc63daf-8483-4772-8e79-3d69d8477de4"), 4096, 30000, "root"),
            ])
            make_image.write_root_fstab(root, partitions[1], partitions[0])
            lines = (root / "etc/fstab").read_text().splitlines()[1:]
            self.assertEqual(lines[0].split(), [f"/dev/disk/by-partuuid/{partitions[1]}", "/", "ext4", "defaults", "0", "1"])
            self.assertEqual(lines[1].split(), [f"/dev/disk/by-partuuid/{partitions[0]}", "/boot", "vfat", "defaults", "0", "2"])
            with image.open("rb") as stream:
                stream.seek(2 * 512 + 16)
                self.assertEqual(uuid.UUID(bytes_le=stream.read(16)), partitions[0])
            executable = root / "gpt-reader"
            subprocess.run(["cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                            "-Ikernel/reliefnt/include", "-Ikernel/reliefnt/include/uapi", "-idirafter", "userland/runtime/include",
                            "tools/tests/rootfs_gpt_test.c", "-o", executable], cwd=ROOT, check=True)
            subprocess.run([executable, image, str(partitions[0]), str(partitions[1])], check=True, timeout=20)


if __name__ == "__main__":
    unittest.main()
