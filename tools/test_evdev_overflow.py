#!/usr/bin/env python3
"""Build the real input.c evdev fan-out code under ASan/UBSan and run regressions."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
INCLUDES = [
    "-Ikernel/reliefnt/include",
    "-Iinclude",
    "-Ikernel/reliefnt/include/uapi",
    "-Ikernel/reliefnt/include",
]


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="reliefos-evdev-overflow-") as directory:
        binary = Path(directory) / "evdev-overflow"
        subprocess.run(
            ["clang", "-std=c11", "-g", "-O1", "-ffunction-sections",
             "-fdata-sections", "-fsanitize=address,undefined",
             "-fno-sanitize-recover=all", *INCLUDES,
             "-Wl,--gc-sections", "tools/tests/evdev_overflow_test.c",
             "-o", str(binary)],
            cwd=ROOT, check=True)
        result = subprocess.run([str(binary)], cwd=ROOT, timeout=30,
                                capture_output=True, text=True)
        print(result.stdout, end="")
        if result.returncode:
            print(result.stderr, end="")
        return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
