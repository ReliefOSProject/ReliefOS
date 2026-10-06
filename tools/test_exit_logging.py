#!/usr/bin/env python3
"""Build and run the task-exit logging contract fixture.

The fixture compiles the real scheduler exit path and the userland exit stub
against a host harness, so a failure means the kernel no longer emits exactly
one exit event per task, emits it while holding the scheduler lock, or loses the
pid/name/code snapshot it captured under that lock.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

INCLUDES = [
    "-Ikernel/reliefnt/include", "-Iinclude",
    "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/kernel/reliefnt/include",
]


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="reliefos-exit-log-") as directory:
        binary = Path(directory) / "task-exit-logging"
        subprocess.run(
            ["clang", "-std=c11", "-g", "-O1",
             "-ffunction-sections", "-fdata-sections",
             "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
             *INCLUDES, "-Wl,--gc-sections",
             "tools/tests/task_exit_logging_test.c", "-o", str(binary)],
            cwd=ROOT, check=True)
        result = subprocess.run([str(binary)], cwd=ROOT, timeout=30)
        print("task-exit-logging: " + ("PASS" if result.returncode == 0 else "FAIL"))
        return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
