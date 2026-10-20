#!/usr/bin/env python3
"""Exercise the real syscall/architecture return path and execution lock."""
from pathlib import Path
import hashlib
import os
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(path, name):
    source = path.read_text()
    match = re.search(r"^[^\n]+\b" + name + r"\([^\n]*\)\n\{", source, re.M)
    if match is None:
        raise RuntimeError("function not found: " + name)
    end = source.index("\n}\n", match.start()) + 3
    print(f"SOURCE {name} sha256={hashlib.sha256(source.encode()).hexdigest()}", flush=True)
    return source[match.start():end]


def main():
    compiler = shutil.which("clang") or shutil.which("cc")
    if compiler is None:
        raise RuntimeError("a C compiler is required")
    kernel = ROOT / "kernel/reliefnt"
    bodies = [
        ("syscall_return_body.inc", "kernel/reliefnt/syscall.c", "syscall_dispatch_frame"),
        ("user_return_body.inc", "kernel/exec/userland.c", "userland_schedule_from_frame"),
        ("int80_return_body.inc", "arch/x86_64/idt.c", "int80_dispatch"),
    ]
    temporary = ROOT / "out/tmp"
    temporary.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="user-return-", dir=temporary) as directory:
        work = Path(directory)
        for target, source, name in bodies:
            (work / target).write_text(function(kernel / source, name))
        source = (kernel / "kernel/reliefnt/syscall.c").read_text()
        start = source.index("    if (number == LINUX_SYS_PSELECT6) {")
        end = source.index("\n    }\n", start) + 6
        (work / "pselect_dispatch_body.inc").write_text(source[start:end])
        env = {**os.environ, "TMPDIR": str(temporary)}
        for fixture in ("user_return_gate_test", "user_return_sched_lock_test", "pselect_mask_test"):
            executable = work / fixture
            subprocess.run([
                compiler, "-std=c11", "-pthread", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-fno-omit-frame-pointer", "-ffunction-sections", "-fdata-sections",
                "-Wl,--gc-sections", "-I" + str(work), "-Ikernel/reliefnt/include", "-Iinclude",
                "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/kernel/reliefnt/include",
                "tools/tests/" + fixture + ".c", "-o", str(executable),
            ], cwd=ROOT, env=env, check=True)
            subprocess.run([str(executable)], cwd=ROOT, env=env, check=True, timeout=30)


if __name__ == "__main__":
    main()
