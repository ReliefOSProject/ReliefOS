#!/usr/bin/env python3
"""Compile and run the host fixture against the real pipe and wait code."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
KERNEL = ROOT / "kernel/reliefnt"
FIXTURE = ROOT / "tools/tests/pipe_wake_test.c"
BASELINE = "50f9f77af4d99094d61025dd3035bea24e487667"
EXPECTED_RED = "8192-byte writer woke before 16 * 512 bytes were available"


def run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        **kwargs,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--verify-red",
        action="store_true",
        help="run the fixture with the pre-optimization pipe implementation and require the threshold regression",
    )
    args = parser.parse_args()

    temp_root = ROOT / "out/tmp"
    temp_root.mkdir(parents=True, exist_ok=True)
    os.environ["TMPDIR"] = str(temp_root)

    with tempfile.TemporaryDirectory(prefix="pipe-wake-", dir=temp_root) as temp_name:
        temp_dir = Path(temp_name)
        binary = temp_dir / "pipe_wake_test"
        command = [
            os.environ.get("CC", "cc"),
            "-std=gnu11",
            "-O1",
            "-g",
            "-Wall",
            "-Wextra",
            "-Wno-unused-parameter",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            "-ffunction-sections",
            "-fdata-sections",
            str(FIXTURE),
            "-Wl,--gc-sections",
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

        if args.verify_red:
            old_source = temp_dir / "syscall_ipc_old.c"
            old = run(
                ["git", "-C", str(KERNEL), "show", f"{BASELINE}:kernel/reliefnt/syscall_ipc.c"]
            )
            if old.returncode:
                sys.stderr.write(old.stdout)
                return old.returncode
            old_source.write_text(old.stdout, encoding="utf-8")
            command.insert(1, f'-DPIPE_WAKE_IPC_SOURCE="{old_source}"')

        compiled = run(command)
        if compiled.returncode:
            sys.stderr.write(compiled.stdout)
            return compiled.returncode

        completed = run([str(binary)])
        if args.verify_red:
            if completed.returncode == 0 or EXPECTED_RED not in completed.stdout:
                sys.stderr.write(completed.stdout)
                sys.stderr.write("expected legacy IPC to fail the 8192-byte threshold assertion\n")
                return 1
            print(f"RED confirmed against {BASELINE[:12]} IPC: {EXPECTED_RED}")
            return 0

        sys.stdout.write(completed.stdout)
        return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
