#!/usr/bin/env python3
"""Exercise the production network adapter and its error propagation on host Linux."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefos-netmand-") as temporary:
    work = Path(temporary)
    headers = work / "include/reliefos"
    headers.mkdir(parents=True)
    for name in ("unix_ipc",):
        (headers / f"{name}.h").symlink_to(ROOT / f"userland/runtime/include/reliefos/{name}.h")
    binary = work / "netmand-test"
    subprocess.run(["cc", "-std=c11", "-O1", "-g", "-D_GNU_SOURCE", "-DRELIEFOS_USE_MUSL",
                    "-fsanitize=address,undefined", "-ffunction-sections", "-fdata-sections",
                    "-Wl,--gc-sections", "-I" + str(work / "include"), "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi",
                    "tools/tests/netmand_client_test.c", "userland/runtime/src/unix_ipc.c",
                    "-o", str(binary)], cwd=ROOT, check=True)
    subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=20)
