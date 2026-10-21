"""Exercise the real AHCI completion loop with controller register fixtures."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefos-ahci-") as directory:
    executable = Path(directory) / "ahci-completion"
    subprocess.run(["cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                    "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                    "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/include",
                    "tools/tests/ahci_completion_test.c", "-o", executable], cwd=ROOT, check=True)
    subprocess.run([executable], check=True, timeout=10)
