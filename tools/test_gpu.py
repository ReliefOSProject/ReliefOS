"""Test the SDK header boundary and native task-manager GPU sampler."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class GpuAbiTests(unittest.TestCase):

    def test_sdk_header_single_source(self):
        # SDK ABI headers come from the headers_install export; the old
        # hand-maintained devtools/include mirrors must not come back.
        self.assertFalse((ROOT / "devtools/include/leonos").exists())
        self.assertFalse((ROOT / "devtools/include/linux").exists())

    def test_taskmgr_sampling(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-gpu-sampler-") as tmp:
            executable = str(Path(tmp) / "sampler")
            subprocess.run([
                "cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-Iinclude",
                "-Ikernel/reliefnt/include/uapi",
                    "tools/tests/taskmgr_gpu_sample_test.c", "-o", executable,
            ], cwd=ROOT, check=True)
            subprocess.run([executable], cwd=ROOT, check=True, timeout=30)



if __name__ == "__main__":
    unittest.main()
