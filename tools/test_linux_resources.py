#!/usr/bin/env python3
"""Exercise real resource-limit syscall helpers under ASan/UBSan."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefos-rlimit-") as directory:
    for test in ("resource_limits", "resource_mm"):
        output = str(Path(directory) / test)
        subprocess.run(["cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                        "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/include",
                        f"tools/tests/{test}_test.c", "-o", output], cwd=root, check=True)
        subprocess.run([output], cwd=root, check=True, timeout=20)
