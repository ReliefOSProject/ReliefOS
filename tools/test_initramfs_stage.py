#!/usr/bin/env python3
"""Inspect real newc bytes and independently exercise early-root command policy."""
import hashlib
import json
from pathlib import Path
import stat
import subprocess
import unittest

import test_kernel_modules_stage as fixtures

ROOT = Path(__file__).resolve().parents[1]


class Initramfs(fixtures.ModulesStage):
    def test_archive_determinism_and_dependency_closure(self):
        self.stage()
        output = self.work / "initramfs.cpio"
        command = [str(fixtures.KERNEL_TOOLS), "initramfs-stage", str(ROOT), str(self.output),
                   str(fixtures.BUSYBOX), str(fixtures.BUSYBOX.with_name("busybox.links")), str(output), "1234567890", str(fixtures.RECOVERY)]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        first = output.read_bytes()
        subprocess.run(command, check=True)
        self.assertEqual(first, output.read_bytes())
        self.assertEqual(len(first) % 512, 0)
        at, entries = 0, {}
        while first[at:at + 6] == b"070701":
            fields = [int(first[at + 6 + i * 8:at + 14 + i * 8], 16) for i in range(13)]
            at += 110
            name = first[at:at + fields[11] - 1].decode()
            self.assertEqual(first[at + fields[11] - 1], 0)
            at = (at + fields[11] + 3) & ~3
            payload = first[at:at + fields[6]]
            entries[name] = (fields, payload)
            at = (at + fields[6] + 3) & ~3
            self.assertEqual(fields[2:4], [0, 0])
            self.assertEqual(fields[5], 1234567890)
            if name == "TRAILER!!!":
                break
        self.assertIn("TRAILER!!!", entries)
        self.assertEqual(entries["init"][1], (ROOT / "system/initramfs/init").read_bytes())
        self.assertEqual(entries["sbin/kernel-bundle"][1], fixtures.RECOVERY.read_bytes())
        self.assertEqual(entries["sbin/kernel-bundle"][0][1] & 0o777, 0o755)
        self.assertEqual(entries["bin/sh"][0][1] & 0o170000, stat.S_IFLNK)
        self.assertEqual(entries["bin/sh"][1], b"busybox")
        self.assertEqual(entries["dev/console"][0][9:11], [5, 1])
        self.assertEqual(entries["etc/modules-load.d/reliefos-boot.conf"][1], b"rootfs\n")
        self.assertIn(f"lib/modules/{fixtures.RELEASE}/kernel/helper.ko", entries)
        self.assertNotIn(f"lib/modules/{fixtures.RELEASE}/kernel/optional.ko", entries)
        metadata = json.loads(Path(str(output) + ".json").read_text())
        self.assertEqual(metadata["initramfs_sha256"], hashlib.sha256(first).hexdigest())
        self.assertEqual(subprocess.run(["sh", str(ROOT / "system/initramfs/init")], capture_output=True).returncode, 77)

    def test_boot_command_policy(self):
        script = '. "$1"; parse_cmdline "$2" || exit 3; printf "%s\\n" "$root" "$final_init" "$root_access" "$rootflags"'
        def parse(text):
            return subprocess.run(["sh", "-c", script, "policy", str(ROOT / "system/initramfs/functions"), text],
                                  text=True, capture_output=True)
        result = parse('root=PARTUUID=1234 init=/sbin/custom ro rootflags=noatime')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), ["PARTUUID=1234", "/sbin/custom", "ro", "noatime"])
        result = parse('root=/ mode=installer rdinit=/bin/sh init=/sbin/init rw')
        self.assertEqual(result.stdout.splitlines()[0], "/dev/ram0")
        self.assertEqual(parse('root="UUID=1234"').stdout.splitlines()[0], "UUID=1234")
        self.assertEqual(parse('root=/dev/sda3 root=/dev/nvme0n1p3 rw ro').stdout.splitlines()[2], "ro")
        for invalid in ('root=PARTUUID=ok init=relative', 'root=../../dev/sda', 'root=/'):
            self.assertNotEqual(parse(invalid).returncode, 0)

    def test_partuuid_root_uses_canonical_hexadecimal_device_identity(self):
        command = '. "$1"; partuuid_device "$2"'
        result = subprocess.run(["sh", "-c", command, "identity", str(ROOT / "system/initramfs/functions"),
                                 "PARTUUID=5C13543B-732C-4F81-8652-621124484420"], text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "/dev/disk/by-partuuid/5c13543b-732c-4f81-8652-621124484420")


if __name__ == "__main__":
    unittest.main()
