#!/usr/bin/env python3
"""Run the userspace NTP origin/era/packet validation with ASan and UBSan."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="ntp-protocol-") as temporary:
    binary = Path(temporary) / "ntp"
    subprocess.run(["cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "tools/tests/ntp_protocol_test.c", "-o", str(binary)], cwd=ROOT, check=True)
    subprocess.run([str(binary)], check=True, timeout=20)
