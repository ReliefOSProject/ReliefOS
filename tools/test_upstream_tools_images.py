#!/usr/bin/env python3
"""Check actual production ext2 images, including both installer payload roots."""
from pathlib import Path
import hashlib
import json
from storage_tools import COMPAT_LINKS
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
STAGE = ROOT / "build/esp"
IMAGES = ((ROOT / "build/live/root.ext2", ("",)),
          (ROOT / "build/install/root.fat", ("", "/install/root")))


def debugfs(image, command):
    return subprocess.check_output(["debugfs", "-R", command, str(image)], stderr=subprocess.DEVNULL)


class ImageToolsTests(unittest.TestCase):
    def test_installer_runtime_contains_advanced_install_guide(self):
        guide = debugfs(ROOT / "build/install/root.fat",
                        "cat /root/installation.rst")
        self.assertEqual(guide, (ROOT / "Documentation/admin-guide/installation.rst").read_bytes())

    def test_iso_payloads_match_tested_roots_and_kernel(self):
        def digest(path):
            with path.open("rb") as source:
                return hashlib.file_digest(source, "sha256").hexdigest()

        evidence = []
        for name, root, config, guest in (
                ("reliefos.iso", "build/live/root.ext2", "boot/grub/live.cfg", "live-smp1"),
                ("reliefos-installer.iso", "build/install/root.fat", "boot/grub/installer.cfg", "install-smp2")):
            iso = ROOT / "build/images" / name
            result = json.loads((ROOT / f"build/storage-upstream-guest/{guest}/result.json").read_text())
            self.assertTrue(result["complete"])
            self.assertEqual(result["failed"], [])
            self.assertEqual(digest(ROOT / root), result["base_sha256"])
            self.assertEqual(digest(ROOT / "build/system/kernel.sys"), result["kernel_sha256"])
            payloads = {}
            with tempfile.TemporaryDirectory(prefix="reliefos-iso-check-") as directory:
                for inside, source in (("/install/root.fat", root),
                                       ("/leonos/kernel.sys", "build/system/kernel.sys"),
                                       ("/grub/grub.cfg", config)):
                    destination = Path(directory) / Path(inside).name
                    subprocess.run(["xorriso", "-osirrox", "on", "-indev", str(iso),
                                    "-extract", inside, str(destination)], check=True,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    payloads[inside] = digest(destination)
                    self.assertEqual(payloads[inside], digest(ROOT / source))
            evidence.append({"iso": str(iso), "sha256": digest(iso), "payloads": payloads,
                             "guest_result": guest, "guest_checks": len(result["passed"])})
        (ROOT / "build/storage-tools-iso-evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")

    def test_embedded_tools_and_library_bytes_match_staging(self):
        paths = ("bin/busybox", "usr/sbin/fdisk", "usr/sbin/sfdisk", "bin/mount", "bin/umount",
                 "bin/lsblk", "usr/sbin/blkid", "usr/sbin/fsck", "usr/sbin/mkfs.ext4",
                 "usr/sbin/fsck.ext4", "usr/sbin/mkfs.ext2", "usr/sbin/fsck.ext2",
                 "usr/sbin/mkfs.fat", "usr/sbin/fsck.fat",
                 "usr/sbin/mkfs.exfat", "usr/sbin/fsck.exfat", "usr/sbin/leonos-grub-installer",
                 "usr/lib/libfdisk.so.1", "usr/lib/libuuid.so.1", "usr/lib/libsmartcols.so.1",
                 "usr/lib/libmount.so.1", "usr/lib/libblkid.so.1")
        for image, prefixes in IMAGES:
            for prefix in prefixes:
                for name in paths:
                    with self.subTest(image=image.name, root=prefix or "/", path=name):
                        source = STAGE / name
                        self.assertTrue(source.is_file(), str(source))
                        actual = source.resolve().relative_to(STAGE)
                        binary = debugfs(image, f"cat {prefix}/{actual}")
                        self.assertEqual(hashlib.sha256(binary).digest(),
                                         hashlib.sha256(source.read_bytes()).digest(), name)

    def test_legacy_paths_resolve_to_official_tools(self):
        links = COMPAT_LINKS
        for image, prefixes in IMAGES:
            for prefix in prefixes:
                for name, target in links.items():
                    with self.subTest(image=image.name, root=prefix or "/", path=name):
                        stat = debugfs(image, f"stat {prefix}/{name}").decode()
                        self.assertIn("Type: symlink", stat)
                        self.assertIn(f'"{target}"', stat)


if __name__ == "__main__":
    unittest.main()
