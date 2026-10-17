#!/usr/bin/env python3
"""Build and run the pthread fixture against the production wait code."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import tempfile
import sys


ROOT = Path(__file__).resolve().parents[1]
KERNEL = ROOT / "kernel/reliefnt"
FIXTURE = ROOT / "tools/tests/wait_queue_race_test.c"


def run(command: list[str], timeout: int) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=ROOT,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
    )


def main() -> int:
    temp_root = ROOT / "out/tmp"
    temp_root.mkdir(parents=True, exist_ok=True)
    os.environ["TMPDIR"] = str(temp_root)

    try:
        with tempfile.TemporaryDirectory(prefix="wait-queue-race-", dir=temp_root) as temp_name:
            binary = Path(temp_name) / "wait_queue_race_test"
            command = [
                os.environ.get("CC", "cc"),
                "-std=gnu11",
                "-O1",
                "-g",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                "-ffunction-sections",
                "-fdata-sections",
                str(FIXTURE),
                "-Wl,--gc-sections",
                "-pthread",
                "-o",
                str(binary),
                "-I",
                str(KERNEL / "kernel/reliefnt/include"),
                "-I",
                str(KERNEL / "include"),
                "-I",
                str(KERNEL / "include/uapi"),
                "-I",
                str(ROOT / "include"),
                "-I",
                str(ROOT / "userland/runtime/include"),
            ]
            compiled = run(command, timeout=30)
            if compiled.returncode:
                sys.stderr.write(compiled.stdout)
                return compiled.returncode

            completed = run([str(binary)], timeout=30)
            sys.stdout.write(completed.stdout)
            return completed.returncode
    except subprocess.TimeoutExpired as error:
        sys.stderr.write(f"wait queue fixture timed out: {error.cmd}\n")
        if error.stdout:
            sys.stderr.write(error.stdout.decode() if isinstance(error.stdout, bytes) else error.stdout)
        return 124


if __name__ == "__main__":
    raise SystemExit(main())
