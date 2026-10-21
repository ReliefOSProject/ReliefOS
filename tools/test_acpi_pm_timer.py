#!/usr/bin/env python3
"""Compile the production ACPI PM clock descriptor regression with sanitizers."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    compiler = shutil.which("clang") or shutil.which("cc")
    if compiler is None:
        raise RuntimeError("a C compiler is required")
    temporary = ROOT / "out/tmp"
    temporary.mkdir(parents=True, exist_ok=True)
    environment = {**os.environ, "TMPDIR": str(temporary)}
    with tempfile.TemporaryDirectory(prefix="reliefos-acpi-pm-",
                                     dir=temporary) as directory:
        executable = str(Path(directory) / "acpi-pm")
        subprocess.run([
            compiler, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            "-fno-omit-frame-pointer", "-ffunction-sections", "-fdata-sections",
            "-Wl,--gc-sections", "-Ikernel/reliefnt/include", "-Iinclude",
            "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/include",
            "tools/tests/acpi_pm_timer_test.c", "-o", executable,
        ], cwd=ROOT, env=environment, check=True)
        subprocess.run([executable], cwd=ROOT, env=environment,
                       check=True, timeout=30)


if __name__ == "__main__":
    main()
