#!/usr/bin/env python3
"""Exercise the real userspace RA DNS parser, without privileged network setup."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    with tempfile.TemporaryDirectory() as temporary:
        binary = Path(temporary) / "rdnss-test"
        subprocess.run([os.environ.get("HOSTCC", "cc"), "-std=c11", "-D_GNU_SOURCE",
                        "-Wall", "-Wextra", "-Werror", "-O2",
                        str(ROOT / "tools/tests/rdnss_test.c"),
                        str(ROOT / "userland/apps/rdnssd/rdnss.c"),
                        "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

if __name__ == "__main__":
    main()
