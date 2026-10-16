#!/usr/bin/env python3
"""Exercise actual vDSO mapping, physical references and initial auxv layout."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    tmp = Path(os.environ.get("TMPDIR", str(ROOT / "out/tmp")))
    tmp.mkdir(parents=True, exist_ok=True)
    for name in ("vdso_mapping", "vdso_auxv"):
        with tempfile.TemporaryDirectory(prefix="reliefos-vdso-map-", dir=tmp) as directory:
            binary = Path(directory) / name
            subprocess.run([
                "cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-Ikernel/reliefnt/include", "-Iinclude",
                "-Ikernel/reliefnt/include/uapi",
                "-Ikernel/reliefnt/kernel/reliefnt/include",
                f"tools/tests/{name}_test.c", "-o", str(binary),
            ], cwd=ROOT, check=True)
            subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=30)


if __name__ == "__main__":
    main()
