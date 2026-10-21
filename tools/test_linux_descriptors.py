#!/usr/bin/env python3
"""Exercise actual descriptor helpers and scheduler table growth with sanitizers."""
from pathlib import Path
import subprocess
import tempfile

from kernel_test_sources import kernel_sources, module_text

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefos-descriptors-") as directory:
    binary = str(Path(directory) / "descriptors")
    subprocess.run([
        "clang", "-std=c11", "-g", "-O1", "-ffunction-sections", "-fdata-sections",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/include", "-Wl,--gc-sections",
        "tools/tests/descriptor_table_test.c", *kernel_sources("scheduler"),
        "kernel/reliefnt/kernel/sync/wait.c", "kernel/reliefnt/kernel/syscall/ipc/sysv_msg.c",
        "kernel/reliefnt/kernel/syscall/ipc/sysv_sem.c", "kernel/reliefnt/kernel/syscall/fs/locks.c",
        "kernel/reliefnt/sound/core/core.c", *kernel_sources("pcm"),
        "kernel/reliefnt/sound/core/device.c", "kernel/reliefnt/sound/core/alsa_control.c",
        "kernel/reliefnt/sound/core/mixer.c", "kernel/reliefnt/sound/core/oss.c",
        "kernel/reliefnt/sound/core/alsa_pcm.c",
        "kernel/reliefnt/sound/core/timer.c",
        "kernel/reliefnt/fs/vfs/object.c",
        "-o", binary,
    ], cwd=ROOT, check=True)
    subprocess.run([binary], cwd=ROOT, check=True, timeout=20)
