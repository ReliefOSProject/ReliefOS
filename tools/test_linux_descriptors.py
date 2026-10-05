#!/usr/bin/env python3
"""Exercise actual descriptor helpers and scheduler table growth with sanitizers."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefos-descriptors-") as directory:
    binary = str(Path(directory) / "descriptors")
    subprocess.run([
        "clang", "-std=c11", "-g", "-O1", "-ffunction-sections", "-fdata-sections",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/kernel/reliefnt/include", "-Wl,--gc-sections",
        "tools/tests/descriptor_table_test.c", "kernel/reliefnt/kernel/reliefnt/sched/sched.c",
        "kernel/reliefnt/kernel/reliefnt/wait.c", "kernel/reliefnt/kernel/reliefnt/syscall_sysv_msg.c",
        "kernel/reliefnt/kernel/reliefnt/syscall_sysv_sem.c", "kernel/reliefnt/kernel/reliefnt/syscall_locks.c",
        "kernel/reliefnt/kernel/reliefnt/audio/core.c", "kernel/reliefnt/kernel/reliefnt/audio/pcm.c",
        "kernel/reliefnt/kernel/reliefnt/audio/device.c", "kernel/reliefnt/kernel/reliefnt/audio/alsa_control.c",
        "kernel/reliefnt/kernel/reliefnt/audio/mixer.c", "kernel/reliefnt/kernel/reliefnt/audio/oss.c",
        "kernel/reliefnt/kernel/reliefnt/audio/alsa_pcm.c",
        "kernel/reliefnt/kernel/reliefnt/audio/timer.c",
        "kernel/reliefnt/fs/object.c",
        "-o", binary,
    ], cwd=ROOT, check=True)
    subprocess.run([binary], cwd=ROOT, check=True, timeout=20)
