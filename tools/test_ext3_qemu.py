#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise the installed runtime ext3 formatter and kernel using a private QEMU disk."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import time

from test_multi_nic_qemu import prepare

ROOT = Path(__file__).resolve().parents[1]
LINUX_GUID = bytes.fromhex("af3dc60f838472478e793d69d8477de4")


def guest_script(_port, modular):
    load = "modprobe ext3\n" if modular else ""
    module_argument = " --modular" if modular else ""
    return f'''#!/bin/sh
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
exec </dev/console >/dev/ttyS0 2>&1
finish() {{
 status=$?
 echo EXT3_USERLAND_DONE:status=$status
 sync
 poweroff -f
 while :; do sleep 1; done
}}
trap finish EXIT
set -eu
echo EXT3_USERLAND_START
{load}
/probe /dev/sda{module_argument}
echo EXT3_USERLAND_QEMU_PASS
'''


def extract_partition(scratch, target):
    """Copy the sole test partition sparsely after validating its GPT extent."""
    with scratch.open("rb") as source:
        source.seek(1024)
        entry = source.read(128)
        if len(entry) != 128 or entry[:16] != LINUX_GUID:
            raise ValueError("scratch GPT does not contain a Linux partition")
        first, last = struct.unpack_from("<QQ", entry, 32)
        disk_bytes = scratch.stat().st_size
        if not first or last < first or (last + 1) * 512 > disk_bytes:
            raise ValueError("scratch GPT partition extent is invalid")
        length = (last - first + 1) * 512
        source.seek(first * 512)
        remaining = length
        with target.open("wb") as destination:
            while remaining:
                chunk = source.read(min(1024 * 1024, remaining))
                if not chunk:
                    raise ValueError("short scratch partition read")
                if any(chunk):
                    destination.write(chunk)
                else:
                    destination.seek(len(chunk), os.SEEK_CUR)
                remaining -= len(chunk)
            destination.truncate(length)
    return first, length


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("install", "busybox", "probe", "mke2fs", "mke2fs-conf", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--modular", action="store_true")
    parser.add_argument("--accel", choices=("kvm", "tcg"), default="kvm")
    parser.add_argument("--timeout", type=float, default=240)
    args = parser.parse_args()
    for name in ("install", "busybox", "probe", "mke2fs", "mke2fs_conf", "output"):
        setattr(args, name, getattr(args, name).resolve())
    if args.output.exists() or not args.output.is_relative_to(ROOT / "out"):
        parser.error("output must be a new directory inside this checkout's out directory")
    manifest = json.loads((args.install / "install.bundle.json").read_text())
    expected = "m" if args.modular else "y"
    if manifest["configuration"].get("EXT3_FS") != expected:
        parser.error(f"install configuration must have CONFIG_EXT3_FS={expected}")
    args.output.mkdir(parents=True)
    scratch = args.output / "scratch.raw"
    with scratch.open("xb") as image:
        image.truncate(384 * 1024 * 1024)

    def stage(root):
        shutil.copy2(args.mke2fs, root / "bin/mke2fs")
        shutil.copy2(args.mke2fs_conf, root / "etc/mke2fs.conf")

    result = {"passed": False, "vmware_verified": False, "physical_hardware_verified": False}
    iso, manifest = prepare(args, 0, script_factory=guest_script, root_hook=stage)
    serial = args.output / "serial.log"
    command = ["qemu-system-x86_64", "-machine", "q35", "-accel", args.accel,
               "-cpu", "host" if args.accel == "kvm" else "max", "-m", "1024", "-smp", "2",
               "-vga", "std", "-display", "none", "-serial", "file:" + str(serial),
               "-cdrom", str(iso), "-boot", "d", "-nic", "none", "-no-reboot",
               "-drive", f"file={scratch},format=raw,if=none,id=scratch",
               "-device", "ide-hd,drive=scratch,bus=ide.0"]
    result.update(command=command, release=manifest["release"],
                  kernel_sha256=manifest["files"]["kernel.sys"]["sha256"],
                  probe_sha256=hashlib.sha256(args.probe.read_bytes()).hexdigest(),
                  mke2fs_sha256=hashlib.sha256(args.mke2fs.read_bytes()).hexdigest())
    with (args.output / "qemu.log").open("w") as log:
        process = subprocess.Popen(command, stdout=log, stderr=log,
                                   env={**os.environ, "TMPDIR": str(ROOT / "out/tmp")})
        try:
            deadline = time.monotonic() + args.timeout
            while process.poll() is None and time.monotonic() < deadline:
                time.sleep(.25)
            if process.poll() is None:
                raise TimeoutError("ext3 guest timed out")
            text = serial.read_text(errors="replace") if serial.exists() else ""
            for marker in ("EXT3_USERLAND_FORMAT_OK", "EXT3_USERLAND_REMOUNT_OK", "EXT3_USERLAND_IO_OK",
                           "EXT3_USERLAND_QEMU_PASS", "EXT3_USERLAND_DONE:status=0"):
                if marker not in text:
                    raise AssertionError("missing guest result " + marker)
            if args.modular:
                for marker in ("EXT3_USERLAND_PINNED_OK", "EXT3_USERLAND_RELOAD_OK"):
                    if marker not in text:
                        raise AssertionError("missing module result " + marker)
            if "EXT3_USERLAND_FAIL" in text or "Kernel Panic" in text or process.returncode:
                raise AssertionError("guest failed")
            first, length = extract_partition(scratch, args.output / "partition.img")
            with (args.output / "e2fsck.log").open("w") as check_log:
                check = subprocess.run(["/usr/sbin/e2fsck", "-f", "-n", str(args.output / "partition.img")],
                                       stdout=check_log, stderr=subprocess.STDOUT, timeout=60)
            if check.returncode:
                raise RuntimeError(f"host e2fsck returned {check.returncode}")
            result.update(passed=True, returncode=process.returncode, e2fsck=check.returncode,
                          partition_start=first, partition_bytes=length)
        except Exception as error:
            result["error"] = repr(error)
            raise
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
