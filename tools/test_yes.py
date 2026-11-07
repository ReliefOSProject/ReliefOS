#!/usr/bin/env python3
"""Exercise GNU yes behavior and its portable full_write fallback."""

from __future__ import annotations

import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
TMP = ROOT / "out" / "tmp"
TRACE_WRITE = re.compile(r'^write\(1, .*,\s*(\d+)\)\s+= ')


def read_prefix(process: subprocess.Popen[bytes], size: int) -> bytes:
    assert process.stdout is not None
    chunks: list[bytes] = []
    remaining = size
    while remaining:
        chunk = process.stdout.read(min(remaining, 65536))
        if not chunk:
            raise AssertionError(f"yes exited before producing {size} bytes")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def check_pattern(binary: Path, operands: list[str], pattern: bytes, size: int) -> None:
    process = subprocess.Popen(
        [str(binary), *operands], stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    try:
        actual = read_prefix(process, size)
        expected = (pattern * ((size + len(pattern) - 1) // len(pattern)))[:size]
        if actual != expected:
            raise AssertionError(f"yes {operands!r} produced corrupted or misframed bytes")
    finally:
        assert process.stdout is not None
        process.stdout.close()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise AssertionError(f"yes {operands!r} did not stop after its reader closed")


def check_epipe_exit(binary: Path) -> None:
    process = subprocess.Popen(
        [str(binary)], stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    assert process.stdout is not None
    process.stdout.close()
    try:
        status = process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
        raise AssertionError("yes hung after stdout closed")
    if status == 0:
        raise AssertionError("yes reported success after stdout closed")


def check_fallback_write_size(binary: Path) -> None:
    strace = shutil.which("strace")
    if strace is None:
        raise AssertionError("strace is required to inspect real fallback write sizes")

    # Failing the fast-path vmsplice makes splice_write return before output,
    # so this measures the actual full_write fallback.  Closing the peer after
    # 1 MiB gives yes a real EPIPE to exit on.
    parent, child = socket.socketpair()
    parent.settimeout(5)
    TMP.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(TMP), LC_ALL="C")
    with tempfile.TemporaryDirectory(prefix="yes-probe-", dir=TMP) as td:
        trace = Path(td) / "writes.log"
        process = subprocess.Popen(
            [
                strace,
                "-qq",
                "-e",
                "inject=vmsplice:error=ENOSYS",
                "-e",
                "trace=write,vmsplice",
                "-o",
                str(trace),
                str(binary),
            ],
            stdout=child,
            stderr=subprocess.DEVNULL,
            env=env,
            close_fds=True,
        )
        child.close()
        received = 0
        try:
            while received < 1024 * 1024:
                chunk = parent.recv(65536)
                if not chunk:
                    break
                received += len(chunk)
        finally:
            parent.close()
        if received < 1024 * 1024:
            process.kill()
            process.wait()
            raise AssertionError(
                f"fallback probe stopped after only {received} bytes; expected 1 MiB"
            )
        try:
            status = process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise AssertionError("yes did not exit after the fallback probe closed stdout")
        if status == 0:
            raise AssertionError("yes reported success after fallback stdout closed")
        lines = trace.read_text(errors="replace").splitlines()

    requests = [int(match.group(1)) for line in lines if (match := TRACE_WRITE.match(line))]
    if not requests:
        raise AssertionError(
            "strace observed no stdout write calls in the fallback probe: "
            + repr(lines[:5])
        )
    largest = max(requests)
    if largest < 64 * 1024:
        raise AssertionError(
            f"full_write fallback requested at most {largest} bytes per write; expected 65536"
        )


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} PATH_TO_GNU_YES", file=sys.stderr)
        return 2
    binary = Path(sys.argv[1]).resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        print(f"not an executable GNU yes binary: {binary}", file=sys.stderr)
        return 2

    check_pattern(binary, [], b"y\n", 3 * 65536 + 17)
    check_pattern(binary, ["alpha", "beta"], b"alpha beta\n", 3 * 65536 + 31)
    check_pattern(binary, [""], b"\n", 2 * 65536 + 19)
    check_pattern(binary, ["", "x"], b" x\n", 2 * 65536 + 23)
    long_pattern = ("L" * 70003 + "\n").encode()
    check_pattern(binary, [long_pattern[:-1].decode()], long_pattern, len(long_pattern) * 2 + 29)
    check_epipe_exit(binary)
    check_fallback_write_size(binary)
    print("GNU yes byte, EPIPE, and 64 KiB fallback checks: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
