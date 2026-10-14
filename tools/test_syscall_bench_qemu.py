#!/usr/bin/env python3
"""Build and run the guest syscall latency/throughput benchmark in QEMU.

Injects tools/tests/syscall_bench.c (static musl) plus a boot-time OpenRC
service into a private copy of the release disk, boots q35/KVM, and parses the
[syscall-bench] serial markers.  Prints one line per measured syscall plus a
before/after-friendly summary.  The input image is never modified.
"""
from __future__ import annotations

import argparse
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / "build" / "syscall-bench"
MUSL_CC = "/usr/bin/x86_64-linux-musl-gcc"
OVMF = "/usr/share/edk2/x64/OVMF.4m.fd"

SERVICE = """#!/sbin/openrc-run
description="ReliefOS syscall benchmark"
depend() { need reliefos-runtime; before reliefos-session; }
start() { /usr/lib/reliefos/tests/syscall-bench > /dev/ttyS0 2>&1; }
"""


def run(cmd, **kw):
    return subprocess.run([str(x) for x in cmd], cwd=ROOT, check=True, **kw)


def build_probe() -> Path:
    WORK.mkdir(parents=True, exist_ok=True)
    probe = WORK / "syscall-bench"
    run([MUSL_CC, "-static", "-O2", "-Wall", "-Wextra",
         "tools/tests/syscall_bench.c", "-o", probe])
    return probe


def stage(image: Path, probe: Path, out: Path) -> Path:
    """Copy the image, then debugfs-inject the probe + service into its root."""
    raw = out / "guest.raw"
    root_image = out / "guest-root.ext4"
    with (out / "stage.log").open("w") as log:
        run(["qemu-img", "convert", "-f", "vmdk", "-O", "raw", str(image), str(raw)],
            stdout=log, stderr=log)
        with raw.open("rb") as disk:
            disk.seek(512)
            header = disk.read(92)
            if header[:8] != b"EFI PART":
                raise ValueError("input has no GPT header")
            entries = struct.unpack_from("<Q", header, 72)[0]
            entry_size = struct.unpack_from("<I", header, 84)[0]
            # Root is the second GPT entry (entry 0 = ESP, entry 1 = ROOT).
            disk.seek(entries * 512 + entry_size)
            entry = disk.read(entry_size)
            first, last = struct.unpack_from("<QQ", entry, 32)
            remaining = (last - first + 1) * 512
            disk.seek(first * 512)
            with root_image.open("wb") as target:
                while remaining:
                    data = disk.read(min(8 * 1024 * 1024, remaining))
                    if not data:
                        raise ValueError("truncated root partition")
                    target.write(data)
                    remaining -= len(data)
        service = out / "reliefos-syscall-bench"
        service.write_text(SERVICE)
        batch = out / "debugfs.commands"
        batch.write_text("\n".join([
            "mkdir /usr/lib/reliefos/tests",
            f'write "{probe}" /usr/lib/reliefos/tests/syscall-bench',
            "set_inode_field /usr/lib/reliefos/tests/syscall-bench mode 0100755",
            f'write "{service}" /etc/init.d/reliefos-syscall-bench',
            "set_inode_field /etc/init.d/reliefos-syscall-bench mode 0100755",
            "symlink /etc/runlevels/default/reliefos-syscall-bench "
            "../../init.d/reliefos-syscall-bench",
        ]) + "\n")
        run(["debugfs", "-w", "-f", str(batch), str(root_image)], stdout=log, stderr=log)
        with raw.open("r+b") as disk, root_image.open("rb") as partition:
            disk.seek(first * 512)
            shutil.copyfileobj(partition, disk, 8 * 1024 * 1024)
    return raw


def boot(raw: Path, serial: Path, timeout: float) -> str:
    accel = ["-enable-kvm", "-cpu", "host"] if Path("/dev/kvm").exists() else ["-cpu", "max"]
    with tempfile.TemporaryDirectory(prefix="reliefos-sysbench-") as directory:
        qmp = str(Path(directory) / "qmp.sock")
        errors = (raw.parent / "qemu.log").open("w")
        proc = subprocess.Popen(
            ["qemu-system-x86_64", "-machine", "q35", *accel, "-m", "4096",
             "-smp", "2,sockets=1,cores=2,threads=1", "-bios", OVMF,
             "-display", "none", "-serial", f"file:{serial}",
             "-snapshot", "-drive", f"file={raw},if=none,id=disk0,format=raw",
             "-device", "ich9-ahci,id=ahci", "-device", "ide-hd,drive=disk0,bus=ahci.0",
             "-qmp", f"unix:{qmp},server=on,wait=off", "-no-reboot", "-no-shutdown"],
            cwd=ROOT, stdout=errors, stderr=errors)
        deadline = time.monotonic() + timeout
        try:
            while time.monotonic() < deadline:
                text = serial.read_text(errors="replace") if serial.exists() else ""
                if "[syscall-bench] DONE" in text or "KERNEL PANIC" in text:
                    break
                if proc.poll() is not None:
                    break
                time.sleep(0.25)
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
    return serial.read_text(errors="replace") if serial.exists() else ""


def parse(text: str) -> int:
    lines = [ln for ln in text.splitlines() if "[syscall-bench]" in ln]
    for ln in lines:
        print(ln[ln.index("[syscall-bench]"):])
    failures = 0
    if not any("DONE failures=0" in ln for ln in lines):
        print("FAIL: no [syscall-bench] DONE failures=0 marker")
        failures += 1
    return failures


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", type=Path,
                    default=ROOT / "out/x86_64/release/images/reliefos.vmdk")
    ap.add_argument("--timeout", type=float, default=120.0)
    args = ap.parse_args()
    if not args.image.is_file():
        print(f"missing disk image: {args.image}", file=sys.stderr)
        return 2
    probe = build_probe()
    with tempfile.TemporaryDirectory(prefix="sysbench-stage-", dir=WORK) as directory:
        raw = stage(args.image, probe, Path(directory))
        serial = WORK / "guest-serial.log"
        serial.write_text("")
        text = boot(raw, serial, args.timeout)
    return parse(text)


if __name__ == "__main__":
    sys.exit(main())
