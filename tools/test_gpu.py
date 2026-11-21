"""Test the published GPU SDK compatibility boundary without a GPU device."""
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

    def test_compatibility_calls_do_not_access_a_device(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-gpu-abi-") as tmp:
            executable = str(Path(tmp) / "gpu_abi_unavailable_test")
            subprocess.run([
                "cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-Iinclude", "-Ikernel/reliefnt/include/uapi",
                "-idirafter", "userland/runtime/include",
                "tools/tests/gpu_abi_unavailable_test.c",
                "userland/runtime/src/gpu.c", "userland/runtime/src/gpu_sdk.c",
                "-Wl,--wrap=open", "-Wl,--wrap=syscall3", "-o", executable,
            ], cwd=ROOT, check=True)
            subprocess.run([executable], cwd=ROOT, check=True, timeout=30)



if __name__ == "__main__":
    unittest.main()
