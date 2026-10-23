#!/usr/bin/env python3
"""Exercise userspace power controls and account-service power dispatch."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory(prefix="reliefos-power-") as directory:
    controls = str(Path(directory) / "controls")
    subprocess.run([
        "cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi", "-idirafter", "userland/runtime/include",
        "tools/tests/power_controls_test.c", "-o", controls,
    ], cwd=ROOT, check=True)
    subprocess.run([controls], cwd=ROOT, check=True, timeout=10)
    daemon = str(Path(directory) / "authd-power")
    subprocess.run([
        "cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-idirafter", "userland/runtime/include", "-idirafter", "include", "-Ikernel/reliefnt/include", "-Ikernel/reliefnt/include/uapi",
        "tools/tests/authd_power_test.c", "-o", daemon,
    ], cwd=ROOT, check=True)
    subprocess.run([daemon], cwd=ROOT, check=True, timeout=10)
    client = str(Path(directory) / "authd-power-client")
    subprocess.run([
        "cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-idirafter", "userland/runtime/include", "-idirafter", "include", "-Ikernel/reliefnt/include", "-Ikernel/reliefnt/include/uapi",
        "tools/tests/authd_power_client_test.c", "-o", client,
    ], cwd=ROOT, check=True)
    subprocess.run([client], cwd=ROOT, check=True, timeout=10)
