#!/usr/bin/env python3
"""Host regression coverage for the production SMP CPU-id selection path."""

from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def available_host_cpus() -> list[int]:
    if not hasattr(os, "sched_getaffinity") or not hasattr(os, "sched_setaffinity"):
        raise RuntimeError("this test requires Linux CPU-affinity controls")
    available = sorted(os.sched_getaffinity(0))
    if not available:
        raise RuntimeError("the process has no available host CPUs")
    return available


def main() -> None:
    available = available_host_cpus()
    # Cover both a low and a high host CPU when possible. Each fixture process
    # remains pinned to one CPU, so its Linux TSC_AUX value cannot migrate.
    host_cpus = sorted({available[0], available[-1]})
    compiler = shutil.which("clang") or shutil.which("cc")
    if compiler is None:
        raise RuntimeError("a C compiler is required for the SMP CPU-id fixture")

    temp_root = ROOT / "out" / "tmp"
    with tempfile.TemporaryDirectory(prefix="reliefos-smp-cpu-id-",
                                     dir=temp_root) as directory:
        executable = str(Path(directory) / "smp-cpu-id")
        env = os.environ.copy()
        env["TMPDIR"] = str(temp_root)
        subprocess.run([
            compiler, "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
            "-Ikernel/reliefnt/include", "-Iinclude",
            "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/kernel/reliefnt/include",
            "tools/tests/smp_cpu_id_test.c", "-o", executable,
        ], cwd=ROOT, env=env, check=True)
        original_affinity = os.sched_getaffinity(0)
        try:
            for cpu in host_cpus:
                os.sched_setaffinity(0, {cpu})
                subprocess.run([executable], cwd=ROOT, env=env, check=True, timeout=20)
        finally:
            os.sched_setaffinity(0, original_affinity)


if __name__ == "__main__":
    main()
