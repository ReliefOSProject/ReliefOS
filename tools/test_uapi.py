#!/usr/bin/env python3
"""Check shared UAPI ownership and the pinned native syscall table."""
from pathlib import Path
import re
import subprocess

from generate_linux_syscalls import header

ROOT = Path(__file__).resolve().parents[1]
assert (ROOT / "kernel/reliefnt/include/uapi/linux/syscall.h").read_text() == header()
numeric_syscall = re.compile(r"^\s*#\s*define\s+(?:SYS_|__NR_|LINUX_SYS_)\w+\s+(?:0x[\da-fA-F]+|\d+)\b", re.M)
for relative in ("kernel/reliefnt/kernel/reliefnt/include/reliefnt/syscall.h", "userland/runtime/include/reliefos/syscall.h",
                 "userland/runtime/include/leonos/syscall.h"):
    assert not numeric_syscall.search((ROOT / relative).read_text()), relative
headers = sorted((ROOT / "kernel/reliefnt/include/uapi").rglob("*.h"))
for language, compiler in (("c", "clang"), ("c++", "clang++")):
    # Separate translation units catch headers that rely on another header's
    # include order, and C++ checks cover SDK application consumers.
    for source in headers:
        subprocess.run([compiler, "-x", language, "-fsyntax-only", "-Werror",
                        "-I", str(ROOT / "kernel/reliefnt/include/uapi"), "-I", str(ROOT / "include"),
                        str(source)], check=True,
                       stdout=subprocess.DEVNULL)
print(f"PASS shared syscall ownership and {len(headers)} standalone C/C++ UAPI headers")
