#!/usr/bin/env python3
"""Ensure userspace wrappers consume kernel syscall numbers without mirrors."""
from pathlib import Path
import re
ROOT = Path(__file__).resolve().parents[1]
numeric_syscall = re.compile(r"^\s*#\s*define\s+(?:SYS_|__NR_|LINUX_SYS_)\w+\s+(?:0x[\da-fA-F]+|\d+)\b", re.M)
for relative in ("userland/runtime/include/reliefos/syscall.h", "userland/runtime/include/leonos/syscall.h"):
    assert not numeric_syscall.search((ROOT / relative).read_text()), relative
print("PASS userspace consumes canonical syscall numbers")
