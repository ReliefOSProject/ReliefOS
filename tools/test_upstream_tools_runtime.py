#!/usr/bin/env python3
"""Host reference tests of real target ELFs; not ReliefOS guest evidence.

Build the selected root first; missing artifacts are failures, not skips.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
STAGE = Path(os.environ.get("LEONOS_UPSTREAM_TEST_ROOT", ROOT / "build/auth-upstream/root")).resolve()
BUSYBOX = Path(os.environ.get("LEONOS_BUSYBOX_TEST_ELF", ROOT / "build/userland/busybox.elf")).resolve()
MUSL = ROOT / "build/musl/sysroot"


class UpstreamRuntimeTests(unittest.TestCase):
    def fdisk(self, *args, input=None):
        return subprocess.run([str(MUSL / "lib/libc.so"), "--library-path",
            ":".join(str(p) for p in (STAGE / "usr/lib", STAGE / "lib", MUSL / "lib")),
            str(STAGE / "usr/sbin/fdisk"), *args], input=input,
            text=True, capture_output=True, timeout=30)

    def test_fdisk_and_libraries_have_no_host_glibc_dependencies(self):
        # Catches libtool install-time relinking against the host /usr/lib.
        for name in ("usr/sbin/fdisk", "usr/lib/libfdisk.so.1",
                     "usr/lib/libsmartcols.so.1", "usr/lib/libuuid.so.1",
                     "usr/lib/libmount.so.1", "usr/lib/libblkid.so.1",
                     "bin/mount", "bin/umount", "bin/lsblk", "usr/sbin/blkid", "usr/sbin/fsck"):
            with self.subTest(elf=name):
                dynamic = subprocess.check_output(["readelf", "-d", str(STAGE / name)], text=True)
                needed = re.findall(r"\(NEEDED\).*?\[(.*?)\]", dynamic)
                self.assertNotIn("libc.so.6", needed)
                self.assertNotIn("ld-linux-x86-64.so.2", needed)
                self.assertNotIn("libleonos.so.2", needed)

    def test_fdisk_version_uses_target_libraries(self):
        result = self.fdisk("--version")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "fdisk from util-linux 2.42.4")

    def test_fdisk_can_write_and_read_a_gpt_on_a_disposable_file(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-fdisk-runtime-") as temporary:
            disk = Path(temporary) / "disk.img"
            with disk.open("wb") as stream:
                stream.truncate(64 * 1024 * 1024)
            result = self.fdisk(str(disk), input="g\nn\n1\n\n+16M\nw\n")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            with disk.open("rb") as stream:
                stream.seek(512)
                self.assertEqual(stream.read(8), b"EFI PART")
            listing = self.fdisk("-l", str(disk))
            self.assertEqual(listing.returncode, 0, listing.stderr)
            self.assertIn("Disklabel type: gpt", listing.stdout)
            self.assertIn("Linux filesystem", listing.stdout)

    def test_busybox_shell_pipeline_and_applet_ownership(self):
        result = subprocess.run([str(BUSYBOX), "sh", "-c", "printf abc | wc -c"],
                                text=True, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "3")
        applets = subprocess.check_output([str(BUSYBOX), "--list"], text=True).splitlines()
        self.assertIn("ash", applets)
        self.assertIn("sh", applets)
        self.assertNotIn("fdisk", applets)


if __name__ == "__main__":
    unittest.main()
