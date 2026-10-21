#!/usr/bin/env python3
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefos-inet-") as work:
    for name in ("udp_socket", "tcp_state", "ntp_protocol"):
        binary = Path(work) / name
        subprocess.run(["cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                        "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/include",
                        f"tools/tests/{name}_test.c", "-o", str(binary)], cwd=ROOT, check=True)
        subprocess.run([str(binary)], check=True, timeout=20)
