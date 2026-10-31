#!/usr/bin/env python3
"""Exercise project boot-payload copying and filesystem tool staging."""
from pathlib import Path
from storage_tools import stage_filesystems
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "userland/storage/leonos-grub-installer"


class BootPayloadTests(unittest.TestCase):
    def test_repeated_staging_preserves_read_only_package_modes(self):
        with tempfile.TemporaryDirectory(prefix="storage-stage-") as temporary:
            source, target = Path(temporary) / "source", Path(temporary) / "stage"
            (source / "usr/sbin").mkdir(parents=True)
            for fs in ("ext2", "fat", "exfat"):
                for verb in ("mkfs", "fsck"):
                    (source / "usr/sbin" / f"{verb}.{fs}").write_bytes(b"official fixture")
            archive = source / "usr/lib/libext2fs.a"
            archive.parent.mkdir()
            archive.write_bytes(b"static library fixture")
            archive.chmod(0o444)
            stage_filesystems(source, target)
            stage_filesystems(source, target)
            self.assertEqual((target / "usr/lib/libext2fs.a").read_bytes(), b"static library fixture")
            self.assertEqual((target / "usr/lib/libext2fs.a").stat().st_mode & 0o777, 0o444)

    def test_boot_payload_is_copied_and_errors_are_not_success(self):
        self.assertTrue(SCRIPT.is_file(), "missing standalone boot payload copier")
        with tempfile.TemporaryDirectory(prefix="boot-copy-") as temporary:
            source = Path(temporary) / "source"
            destination = Path(temporary) / "ESP with spaces"
            destination.mkdir()
            files = ("EFI/BOOT/BOOTX64.EFI", "reliefos/kernel.sys", "leonos/kernel.sys",
                     "grub/grub.cfg", "grub/fonts/font.pf2")
            for name in files:
                p = source / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(name)
            def copy(src, dest=destination):
                return subprocess.run(["/bin/sh", str(SCRIPT), "--source", str(src), str(dest)],
                                      capture_output=True, text=True, timeout=10)
            (destination / "loader.elf").write_text("preserve until inputs validated")
            self.assertNotEqual(copy(source / "missing").returncode, 0)
            self.assertNotEqual(copy(source, destination / "missing").returncode, 0)
            (source / "leonos/kernel.sys").unlink()
            self.assertNotEqual(copy(source).returncode, 0)
            self.assertEqual((destination / "loader.elf").read_text(), "preserve until inputs validated")
            (source / "leonos/kernel.sys").write_text("leonos/kernel.sys")
            result = copy(source)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for name in files:
                self.assertEqual((destination / name).read_text(), name)
            self.assertFalse((destination / "loader.elf").exists())
            no_args = subprocess.run(["/bin/sh", str(SCRIPT)], capture_output=True, timeout=10)
            self.assertNotEqual(no_args.returncode, 0)


if __name__ == "__main__":
    unittest.main()
