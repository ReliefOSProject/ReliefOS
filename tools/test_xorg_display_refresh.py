#!/usr/bin/env python3
"""Run production SVGA/framebuffer and timer-cadence behavior fixtures."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_svga import SvgaTests

ROOT = Path(__file__).resolve().parents[1]


class DisplayRefreshTests(unittest.TestCase):
    def test_production_svga_framebuffer_backpressure_and_trace_policy(self):
        SvgaTests("test_driver").test_driver()

    def test_production_timekeeper_uses_50hz_refresh_cadence(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-display-tick-") as directory:
            executable = Path(directory) / "timekeeper-test"
            subprocess.run([
                "cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi",
                "-Ikernel/reliefnt/kernel/reliefnt/include",
                "tools/tests/timekeeper_discipline_test.c",
                "kernel/reliefnt/kernel/reliefnt/time_discipline.c",
                "-o", str(executable),
            ], cwd=ROOT, check=True)
            subprocess.run([str(executable)], cwd=ROOT, check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
