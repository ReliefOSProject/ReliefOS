#!/usr/bin/env python3
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory(prefix="reliefos-framebuffer-fifo-") as directory:
    binary = pathlib.Path(directory) / "framebuffer-fifo-test"
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-Ikernel/reliefnt/kernel/reliefnt/include",
        "-Ikernel/reliefnt/drivers/bootstrap",
        "tools/tests/framebuffer_fifo_test.c", "-o", str(binary),
    ], cwd=ROOT, check=True)
    subprocess.run([str(binary)], check=True)
