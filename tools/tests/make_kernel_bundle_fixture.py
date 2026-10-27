#!/usr/bin/env python3
"""Build real native bundle metadata for OS packaging/layout tests only."""
from pathlib import Path
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import test_kernel_modules_stage as fixtures

root = Path(__file__).resolve().parents[2]
destination, kernel = map(lambda value: Path(value).resolve(), sys.argv[1:])
assert not destination.exists() and destination != Path("/")
fixture = fixtures.ModulesStage("test_real_metadata_dependency_closure_and_stale_output")
fixture.setUp()
try:
    fixture.stage()
    archive = fixture.work / "initramfs.cpio"
    subprocess.run([fixtures.KERNEL_TOOLS, "initramfs-stage", root, fixture.output, fixtures.BUSYBOX,
                    fixtures.BUSYBOX.with_name("busybox.links"), archive, "0", fixtures.RECOVERY], check=True)
    subprocess.run([fixtures.KERNEL_TOOLS, "kernel-payload", fixture.install, fixture.output,
                    archive, destination], check=True)
    shutil.copyfile(fixture.install / "kernel.sys", kernel)
finally:
    fixture.doCleanups()
