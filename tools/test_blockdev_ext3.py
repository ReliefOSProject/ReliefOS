#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise ext3 runtime helpers on disposable files with real e2fsprogs."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    temporary = Path(os.environ.get("TMPDIR", str(ROOT / "out/tmp")))
    temporary.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="reliefos-ext3-userland-", dir=temporary) as directory:
        work = Path(directory)
        binary, volume, disk = work / "probe", work / "volume.img", work / "disk.img"
        subprocess.run(["cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g", "-Wall", "-Wextra",
                        "-Werror", "-Wno-address-of-packed-member", "-Iuserland/runtime/include", "-Iinclude",
                        "-Ikernel/reliefnt/include/uapi", "-Wl,--wrap=ioctl,--wrap=execv",
                        "tools/tests/blockdev_ext3_test.c", "userland/runtime/src/blockdev.c",
                        "-o", str(binary)], cwd=ROOT, check=True)
        subprocess.run([str(binary), str(volume), str(disk)], check=True, timeout=60)
        subprocess.run(["/usr/sbin/e2fsck", "-f", "-n", str(volume)], check=True, timeout=60)
        model = work / "diskmgr-model"
        subprocess.run(["c++", "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror",
                        "-Iuserland/runtime/include", "tools/tests/diskmgr_ext3_test.cpp",
                        "userland/apps/diskmgr/model.cpp", "-o", str(model)], cwd=ROOT, check=True)
        subprocess.run([str(model)], check=True, timeout=10)


if __name__ == "__main__":
    main()
