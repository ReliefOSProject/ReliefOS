#!/usr/bin/env python3
"""Build the production fbdev mode validator against a deterministic device model."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    temp_parent = ROOT / "out/tmp"
    temp_parent.mkdir(parents=True, exist_ok=True)
    os.environ["TMPDIR"] = str(temp_parent)
    with tempfile.TemporaryDirectory(prefix="reliefos-fbdev-", dir=temp_parent) as directory:
        binary = Path(directory) / "fbdev-abi"
        command = [
            "clang", "-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
            "-Wno-unused-function",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            "-Ikernel/reliefnt/include", "-Ikernel/reliefnt/include/uapi",
            "-Ikernel/reliefnt/kernel/reliefnt/include", "-Iinclude",
            "tools/tests/fbdev_abi_test.c",
            "kernel/reliefnt/drivers/video/framebuffer/fbdev.c",
            "-o", str(binary),
        ]
        print("$ " + " ".join(command), flush=True)
        subprocess.run(command, cwd=ROOT, check=True)
        result = subprocess.run([str(binary)], cwd=ROOT, capture_output=True,
                                text=True, timeout=30)
        print(result.stdout, end="")
        if result.returncode:
            print(result.stderr, end="")
            return result.returncode
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
