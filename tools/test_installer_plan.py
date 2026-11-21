"""Verify deferred installer edits with the production GPT writer and real images."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


class InstallerPlanTests(unittest.TestCase):
    def test_drafts_preserve_media_until_confirmed_and_reject_stale_targets(self):
        with tempfile.TemporaryDirectory(prefix="installer-plan-", dir=os.environ.get("TMPDIR")) as directory:
            work = Path(directory)
            executable = work / "plan"
            subprocess.run([
                "cc", "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections,--wrap=open",
                "-idirafter", "userland/runtime/include", "-Ikernel/reliefnt/include",
                "-Ikernel/reliefnt/include/uapi", "tools/tests/installer_plan_test.c",
                "userland/apps/installer/installer_plan.c", "userland/apps/installer/installer_partitions.c",
                "-o", executable,
            ], cwd=ROOT, check=True)
            base = work / "base.raw"
            with base.open("wb") as stream:
                stream.truncate(768 * 1024**2)
            subprocess.run([executable, base, "prepare"], check=True, timeout=20)
            for case in ("preview", "initialize-preview", "readonly", "mounted", "stale-gpt", "stale-fs", "read-error-load", "apply",
                         "bios-clear-preview", "bios-then-data-preview", "bios-delete-preview", "bios-clear-apply"):
                image = work / f"{case}.raw"
                subprocess.run(["cp", "--sparse=always", base, image], check=True)
                before = digest(image)
                subprocess.run([executable, image, case], check=True, timeout=30,
                               env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
                if case in ("preview", "initialize-preview", "readonly", "mounted",
                            "bios-clear-preview", "bios-then-data-preview", "bios-delete-preview"):
                    self.assertEqual(digest(image), before, case)
                elif case.startswith("stale-") or case == "read-error-load":
                    # Exactly the deliberately injected external byte may change.
                    offset = (512 + 16 if case == "read-error-load" else
                              512 + 56 if case == "stale-gpt" else 386 * 1024**2 + 1024)
                    with image.open("r+b") as stream, base.open("rb") as original:
                        stream.seek(offset)
                        original.seek(offset)
                        previous = original.read(1)
                        self.assertEqual(stream.read(1), bytes([previous[0] ^ 0xff]))
                        stream.seek(offset)
                        stream.write(previous)
                    self.assertEqual(digest(image), before, case)
                elif case == "bios-clear-apply":
                    table = json.loads(subprocess.check_output(["sfdisk", "--json", image]))["partitiontable"]
                    self.assertEqual(len(table["partitions"]), 4)
                    self.assertEqual(table["partitions"][3]["type"].lower(),
                                     "21686148-6449-6e6f-744e-656564454649")
                    # The root partition is preserved when a separate unknown
                    # data extent is explicitly reserved and cleared for BIOS.
                    root = table["partitions"][2]
                    with image.open("rb") as stream, base.open("rb") as original:
                        offset = root["start"] * table["sectorsize"]
                        stream.seek(offset)
                        original.seek(offset)
                        for _ in range(root["size"] * table["sectorsize"] // (1024 * 1024)):
                            self.assertEqual(stream.read(1024 * 1024), original.read(1024 * 1024))
                    subprocess.run(["sfdisk", "--verify", image], check=True)
                else:
                    table = json.loads(subprocess.check_output(["sfdisk", "--json", image]))["partitiontable"]
                    self.assertEqual(len(table["partitions"]), 4)
                    self.assertIn("Keep Data", [part["name"] for part in table["partitions"]])
                    subprocess.run(["sfdisk", "--verify", image], check=True)
                with image.open("rb") as stream:
                    stream.seek(386 * 1024**2 + 8192)
                    self.assertEqual(stream.read(23), bytes(23) if case == "bios-clear-apply"
                                     else b"PRESERVE-EXISTING-DATA\0")
                image.unlink()


if __name__ == "__main__":
    unittest.main()
