#!/usr/bin/env python3
"""Check that APK kernel payloads reject mixed or damaged boot components."""
import json
from pathlib import Path
import subprocess
import unittest
import test_kernel_modules_stage as fixtures

ROOT = Path(__file__).resolve().parents[1]


class KernelPayload(unittest.TestCase):
    def test_pairing_and_failed_update_preserves_owned_stage(self):
        fixture = fixtures.ModulesStage("test_real_metadata_dependency_closure_and_stale_output")
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        fixture.stage()
        archive = fixture.work / "initramfs.cpio"
        subprocess.run([str(fixtures.KERNEL_TOOLS), "initramfs-stage", str(ROOT), str(fixture.output),
                        str(fixtures.BUSYBOX), str(fixtures.BUSYBOX.with_name("busybox.links")), str(archive), "0", str(fixtures.RECOVERY)], check=True)
        output = fixture.work / "payload"
        command = [str(fixtures.KERNEL_TOOLS), "kernel-payload", str(fixture.install),
                   str(fixture.output), str(archive), str(output)]
        subprocess.run(command, check=True)
        before = (output / "kernel-payload.json").read_bytes()
        manifest = json.loads(before)
        self.assertEqual(manifest["release"], fixtures.RELEASE)
        self.assertEqual((output / manifest["initramfs"]).read_bytes(), archive.read_bytes())
        archive.write_bytes(archive.read_bytes() + b"damaged")
        result = subprocess.run(command, text=True, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("checksum", result.stderr)
        self.assertEqual((output / "kernel-payload.json").read_bytes(), before)
        metadata = Path(str(archive) + ".json")
        data = json.loads(metadata.read_text())
        data["release"] = "different"
        metadata.write_text(json.dumps(data))
        result = subprocess.run(command, text=True, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("identity mismatch", result.stderr)
        self.assertEqual((output / "kernel-payload.json").read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
