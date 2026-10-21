#!/usr/bin/env python3
"""Run the production timekeeper through host-side clock transition checks."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    tmp = Path(os.environ.get("TMPDIR", str(ROOT / "out/tmp")))
    tmp.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="reliefos-vdso-time-", dir=tmp) as directory:
        binary = Path(directory) / "timekeeper-test"
        subprocess.run(
            [
                "cc", "-std=c11", "-O1", "-g",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-pthread",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-DRELIEFNT_TIME_HOST_TEST",
                "-Ikernel/reliefnt/include", "-Iinclude",
                "-Ikernel/reliefnt/include/uapi",
                "-Ikernel/reliefnt/include",
                "tools/tests/vdso_timekeeper_test.c",
                "kernel/reliefnt/kernel/time/time_discipline.c",
                "-o", str(binary),
            ],
            cwd=ROOT,
            check=True,
        )
        subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=15)


if __name__ == "__main__":
    main()
