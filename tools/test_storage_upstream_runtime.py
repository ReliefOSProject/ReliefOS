#!/usr/bin/env python3
"""Exercise official target tools on disposable files, never host devices."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import sys

ROOT = Path(__file__).resolve().parents[1]
FORMATTERS = Path(os.environ.get("LEONOS_STORAGE_TEST_ROOT", ROOT / "out/x86_64/release/rootfs/raw")).resolve()
UTIL = Path(os.environ.get("LEONOS_UPSTREAM_TEST_ROOT", ROOT / "out/x86_64/release/rootfs/raw")).resolve()
MUSL = ROOT / "out/x86_64/release/sysroot/musl"
_parser = argparse.ArgumentParser(add_help=False)
_parser.add_argument("--filesystem", choices=("ext2", "ext4"), default="ext4")
_args, _remaining = _parser.parse_known_args()
sys.argv = [sys.argv[0], *_remaining]


class StorageRuntimeTests(unittest.TestCase):
    def tool(self, root, command, *args):
        binary = root / command
        self.assertTrue(binary.is_file(), f"missing official tool: {binary}")
        # Formatters are static; util-linux uses the matching target loader.
        argv = [str(binary)]
        if command in ("bin/mount", "bin/umount", "bin/lsblk", "usr/sbin/fsck", "usr/sbin/blkid"):
            argv = [str(MUSL / "lib/libc.so"), "--library-path",
                    ":".join(str(p) for p in (UTIL / "usr/lib", UTIL / "lib", MUSL / "lib")), *argv]
        return subprocess.run([*argv, *map(str, args)], env={**os.environ, "LC_ALL": "C"},
                              capture_output=True, text=True, timeout=60)

    def test_filesystem_roundtrips_and_real_corruption_detection(self):
        filesystems = [("ext4", "ext4", ("-F", "-q"))] if _args.filesystem == "ext4" else [
            ("ext2", "ext2", ("-F", "-q")), ("fat", "vfat", ("-F", "32")), ("exfat", "exfat", ())]
        for fs, kind, mkfs_args in filesystems:
            with self.subTest(fs=fs), tempfile.TemporaryDirectory(prefix="reliefos-fs-") as directory:
                disk = Path(directory) / "filesystem.img"
                with disk.open("wb") as stream:
                    stream.truncate(128 << 20)
                made = self.tool(FORMATTERS, f"usr/sbin/mkfs.{fs}", *mkfs_args, disk)
                self.assertEqual(made.returncode, 0, made.stdout + made.stderr)
                check = self.tool(FORMATTERS, f"usr/sbin/fsck.{fs}", "-n", disk)
                self.assertEqual(check.returncode, 0, check.stdout + check.stderr)
                probe = self.tool(UTIL, "usr/sbin/blkid", "-p", "-s", "TYPE", "-o", "value", disk)
                self.assertEqual(probe.returncode, 0, probe.stderr)
                self.assertEqual(probe.stdout.strip(), kind)
                broken = Path(directory) / "invalid.img"
                with broken.open("wb") as stream:
                    stream.truncate(128 << 20)
                check = self.tool(FORMATTERS, f"usr/sbin/fsck.{fs}", "-n", broken)
                self.assertNotEqual(check.returncode, 0, "fsck must reject an unformatted image")

    def test_mount_and_umount_are_real_upstream_programs(self):
        for command in ("mount", "umount"):
            result = self.tool(UTIL, f"bin/{command}", "--version")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("util-linux 2.42.4", result.stdout)
        listing = self.tool(UTIL, "bin/mount")
        self.assertEqual(listing.returncode, 0, listing.stderr)
        self.assertIn(" on /", listing.stdout)

    def test_lsblk_returns_structured_inventory(self):
        result = self.tool(UTIL, "bin/lsblk", "--json", "--output", "NAME,TYPE")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIsInstance(json.loads(result.stdout)["blockdevices"], list)

    def test_fsck_dry_run_resolves_a_filesystem_checker(self):
        result = self.tool(UTIL, "usr/sbin/fsck", "-N", "-t", "ext4", "/dev/nonexistent-reliefos-test")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("fsck.ext4", result.stdout)


if __name__ == "__main__":
    unittest.main()
