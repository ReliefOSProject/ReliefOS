#!/usr/bin/env python3
"""Check that the Xorg probe uses only the public Linux VT ABI."""
from pathlib import Path
import argparse
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "tools/tests/xorg_vt_abi_test.c"
ALLOWED_INCLUDES = {"fcntl.h", "linux/kd.h", "linux/vt.h", "sys/ioctl.h", "unistd.h"}
REQUIRED = [
    "/dev/tty0", "VT_GETSTATE", "VT_OPENQRY", "VT_GETMODE", "VT_SETMODE",
    "VT_RELDISP", "KDGKBMODE", "KDSKBMODE", "KDSETMODE", "KDGETMODE", "ioctl(",
]
FORBIDDEN = ["RELIEFOS_", "LE" + "ONOS_", "syscall(", "struct vt_mode {"]


def check_source() -> tuple[bool, list[str]]:
    source = PROBE.read_text()
    includes = set(re.findall(r"#include\s*<([^>]+)>", source))
    missing = [token for token in REQUIRED if token not in source]
    forbidden = [token for token in FORBIDDEN if token in source]
    errors = []
    if includes - ALLOWED_INCLUDES:
        errors.append("non-public includes: " + ", ".join(sorted(includes - ALLOWED_INCLUDES)))
    if missing:
        errors.append("missing ABI calls: " + ", ".join(missing))
    if forbidden:
        errors.append("forbidden private ABI: " + ", ".join(forbidden))
    if re.search(r"ioctl\s*\([^,]+,\s*(?:0x[0-9a-fA-F]+|[0-9]+)", source):
        errors.append("numeric ioctl command")
    return not errors, errors or [f"{len(includes)} public includes and {len(REQUIRED)} ABI calls verified"]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-only", action="store_true")
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    ok, notes = check_source()
    print("xorg-vt-abi source-check: " + ("PASS" if ok else "FAIL"))
    for note in notes:
        print("  " + note)
    if args.source_only:
        return 0 if ok else 1
    with tempfile.TemporaryDirectory(prefix="reliefos-vt-abi-") as directory:
        output = Path(directory) / "vt-probe"
        result = subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                 "-I", str(ROOT / "kernel/reliefnt/include/uapi"),
                                 str(PROBE), "-o", str(output)], capture_output=True, text=True)
        print("xorg-vt-abi compile: " + ("PASS" if result.returncode == 0 else "FAIL"))
        if result.returncode:
            print(result.stderr.rstrip())
    return 0 if ok and result.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
