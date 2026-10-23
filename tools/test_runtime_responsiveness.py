"""Focused runtime fbdev capability and mapping regressions."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RuntimeResponsivenessTests(unittest.TestCase):
    def test_framebuffer_capabilities_and_remapping(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-fbdev-") as tmp:
            executable = str(Path(tmp) / "fbdev")
            subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-idirafter", "userland/runtime/include", "-idirafter", "include",
                "-Ikernel/reliefnt/include/uapi", "tools/tests/fbdev_runtime_test.c",
                "-o", executable,
            ], cwd=ROOT, check=True)
            subprocess.run([executable], cwd=ROOT, check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
