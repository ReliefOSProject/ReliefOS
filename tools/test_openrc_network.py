#!/usr/bin/env python3
"""Execute the kernel packet/interface implementations with ASan and UBSan."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    with tempfile.TemporaryDirectory(prefix="reliefos-openrc-network-") as directory:
        for name in ("net_interface", "net_packet", "time_discipline", "timekeeper_discipline", "driver_control"):
            binary = Path(directory) / name
            subprocess.run(["cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/include",
                f"tools/tests/{name}_test.c",
                *(["kernel/reliefnt/kernel/time/time_discipline.c"] if name == "timekeeper_discipline" else []),
                "-o", str(binary)], cwd=ROOT, check=True)
            subprocess.run([str(binary)], check=True, timeout=15)

if __name__ == "__main__":
    main()
