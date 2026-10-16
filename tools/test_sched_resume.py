#!/usr/bin/env python3
"""Host regression coverage for user-return fast resume and EEVDF accounting."""

from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run_host_fixture() -> None:
    compiler = shutil.which("clang") or shutil.which("cc")
    if compiler is None:
        raise RuntimeError("a C compiler is required for the scheduler host fixture")
    with tempfile.TemporaryDirectory(prefix="reliefos-sched-resume-",
                                     dir=ROOT / "out" / "tmp") as directory:
        executable = str(Path(directory) / "sched-resume")
        env = os.environ.copy()
        env["TMPDIR"] = str(ROOT / "out" / "tmp")
        subprocess.run([
            compiler, "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer", "-ffunction-sections", "-fdata-sections",
            "-Wl,--gc-sections", "-Ikernel/reliefnt/include", "-Iinclude",
            "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/kernel/reliefnt/include",
            "tools/tests/sched_resume_test.c", "-o", executable,
        ], cwd=ROOT, env=env, check=True)
        subprocess.run([executable], cwd=ROOT, env=env, check=True, timeout=30)


if __name__ == "__main__":
    run_host_fixture()
