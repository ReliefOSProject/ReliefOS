#!/usr/bin/env python3
"""Run production read/write backends with real pipe queues and wait handling."""
from pathlib import Path
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r"^(?:static )?[^\n;{}]+\b" + name + r"\([^;{}]*\)\n\{", source, re.M)
    if match is None:
        raise RuntimeError("function not found: " + name)
    end = source.index("\n}\n", match.start()) + 3
    return source[match.start():end]


def io_backend(source, operation):
    name = "syscall_io_" + operation
    if re.search(r"^static int64_t " + name + r"\(", source, re.M):
        return function(source, name)
    # Characterize the old backend without copying it into the fixture. This
    # option also permits semantic comparisons with archived pre-extraction code.
    start = source.index("    if (number == LINUX_SYS_" + operation.upper() + ") {\n")
    end = source.index("\n    }\n", start)
    body = source[source.index("\n", start) + 1:end]
    body = "\n".join(line[4:] if line.startswith("    ") else line for line in body.splitlines())
    return "static int64_t " + name + "(uint64_t a0, uint64_t a1, uint64_t a2)\n{\n" + body + "\n}\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "kernel/reliefnt/kernel/reliefnt/syscall.c")
    args = parser.parse_args()
    source = args.source.read_text()
    compiler = shutil.which("clang") or shutil.which("cc")
    if not compiler:
        raise RuntimeError("a C compiler is required")
    temporary = Path(os.environ.get("TMPDIR", str(ROOT / "out/tmp"))).resolve()
    temporary.mkdir(parents=True, exist_ok=True)
    print("SOURCE syscall.c sha256=" + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="syscall-io-", dir=temporary) as directory:
        work = Path(directory)
        start = source.index("struct task_timerfd {")
        end = source.index("\n};", start) + 3
        bodies = [source[start:end]]
        for name in ("copy_text", "task_file_for_io", "task_pty_for_io", "task_pty_status", "file_can_read", "file_can_write"):
            bodies.append(function(source, name))
        bodies.extend(io_backend(source, operation) for operation in ("read", "write"))
        (work / "syscall_io_body.inc").write_text("\n\n".join(bodies))
        for optimization in ("-O0", "-O2"):
            executable = work / ("syscall_io" + optimization)
            subprocess.run([
                compiler, "-std=gnu11", optimization, "-g", "-Wall", "-Wextra",
                "-Werror", "-Wno-unused-parameter",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-fno-omit-frame-pointer", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-I" + str(work), "-Ikernel/reliefnt/kernel/reliefnt/include",
                "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi",
                "-Iuserland/runtime/include", "tools/tests/syscall_io_test.c", "-o", str(executable),
            ], cwd=ROOT, env={**os.environ, "TMPDIR": str(temporary)}, check=True)
            subprocess.run([str(executable)], cwd=ROOT, check=True, timeout=30)
        # The same POSIX program is staged into the guest for runtime checks.
        # This run is its Linux reference, not a kernel-host-fixture result.
        executable = work / "io_runtime_linux_reference"
        subprocess.run([
            compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            "tools/tests/syscall_io_runtime_probe.c", "-o", str(executable),
        ], cwd=ROOT, check=True)
        print("Linux reference:", flush=True)
        subprocess.run([str(executable)], cwd=ROOT,
                       env={**os.environ, "TMPDIR": str(temporary)}, check=True, timeout=30)


if __name__ == "__main__":
    main()
