"""Focused regressions for desktop/terminal runtime stalls."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RuntimeResponsivenessTests(unittest.TestCase):
    def test_framebuffer_reports_hardware_limits_and_remaps_after_mode_change(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-wind-fb-") as tmp:
            executable = str(Path(tmp) / "wind-framebuffer")
            subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-idirafter", "userland/runtime/include", "-idirafter", "include", "-Ikernel/reliefnt/include", "-Ikernel/reliefnt/include/uapi",
                "tools/tests/wind_framebuffer_test.c", "-o", executable,
            ], cwd=ROOT, check=True)
            subprocess.run([executable], cwd=ROOT, check=True, timeout=10)

    def test_window_repaints_reuse_live_shared_memory(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-wind-surface-") as tmp:
            executable = str(Path(tmp) / "wind-surface")
            subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-idirafter", "userland/runtime/include", "-idirafter", "include", "-Ikernel/reliefnt/include", "-Ikernel/reliefnt/include/uapi",
                "tools/tests/wind_surface_test.c", "-o", executable,
            ], cwd=ROOT, check=True)
            subprocess.run([executable], cwd=ROOT, check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
