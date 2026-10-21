#!/usr/bin/env python3
"""Deterministic EEVDF tests using the kernel's actual integer policy."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
tmp_root = ROOT / "out" / "tmp"
tmp_root.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="reliefos-eevdf-", dir=tmp_root) as tmp:
    for name in ("eevdf", "eevdf_scheduler", "eevdf_rbtree"):
        binary = str(Path(tmp) / name)
        subprocess.run(["cc", "-std=c11", "-O2", "-g", "-Wall", "-Wextra",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-fno-pie", "-no-pie",
                    "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                    "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/include",
                    f"tools/tests/{name}_test.c", "-o", binary],
                   cwd=ROOT, check=True)
        subprocess.run([binary], check=True, timeout=30)
