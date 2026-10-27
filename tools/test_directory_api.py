#!/usr/bin/env python3
"""Run the actual OS directory adapter against host Linux, not a syscall mock."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DirectoryAPI(unittest.TestCase):
    def test_real_linux_directory(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-directory-api-") as directory:
            work = Path(directory)
            source = ROOT / "userland/runtime/src/fs_directory.c"
            binary = work / "directory-api"
            subprocess.run([*shlex.split(os.environ.get("HOSTCC", "cc")),
                            "-std=c11", "-D_GNU_SOURCE", "-DRELIEFOS_USE_MUSL",
                            "-O1", "-g", "-ffunction-sections", "-fdata-sections",
                            "-Wl,--gc-sections", "-fsanitize=address,undefined",
                            "-Iinclude", "-Ikernel/reliefnt/include/uapi",
                            "-idirafter", "userland/runtime/include", str(source),
                            "tools/tests/directory_api_test.c", "-o", str(binary)],
                           cwd=ROOT, check=True)
            data = work / "data"
            data.mkdir()
            subprocess.run([binary, data], check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
