#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Measure verified TCP echo, guest CPU accounting and UDP latency in QEMU."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

from test_e1000e_qemu import echo_peers
from test_multi_nic_qemu import prepare

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("install", "busybox", "probe", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--modular", action="store_true")
    parser.add_argument("--expect-msix", action="store_true")
    parser.add_argument("--bytes", type=int, default=8 * 1024 * 1024)
    parser.add_argument("--rounds", type=int, default=2)
    args = parser.parse_args()
    for name in ("install", "busybox", "probe", "output"):
        setattr(args, name, getattr(args, name).resolve())
    if args.output.exists() or not args.output.is_relative_to(ROOT / "out"):
        parser.error("output must be a new directory within this checkout's out")
    if not 1 <= args.rounds <= 5 or not 1 <= args.bytes <= 1024 ** 3:
        parser.error("rounds/bytes exceed the bounded probe limits")
    args.output.mkdir(parents=True)
    result = {"passed": False, "platform": "QEMU/KVM", "vmware_verified": False}
    with echo_peers() as (port, counts):
        def script(port, modular):
            load = "modprobe vmxnet3\n" if modular else ""
            return f'''#!/bin/sh
export PATH=/bin
exec </dev/console >/dev/ttyS0 2>&1
finish() {{
 status=$?
 echo VMXNET3_PERF_DONE:status=$status
 poweroff -f
}}
trap finish EXIT
set -eu
{load}udhcpc -f -n -q -i eth0 -s /dhcp-script -t 4 -T 1
for round in $(seq 1 {args.rounds}); do
 /probe eth0 10.0.2.2 {port} {args.bytes}
done
cat /proc/net/dev /proc/interrupts
'''
        iso, manifest = prepare(args, port, script_factory=script)
        serial = args.output / "serial.log"
        command = ["qemu-system-x86_64", "-machine", "q35", "-accel", "kvm", "-cpu", "host",
                   "-m", "1024", "-smp", "2", "-vga", "std", "-display", "none",
                   "-serial", "file:" + str(serial), "-cdrom", str(iso), "-boot", "d",
                   "-netdev", "user,id=net0", "-device", "vmxnet3,netdev=net0", "-no-reboot"]
        result.update(command=command, release=manifest["release"],
                      kernel_sha256=manifest["files"]["kernel.sys"]["sha256"],
                      probe_sha256=hashlib.sha256(args.probe.read_bytes()).hexdigest())
        try:
            with (args.output / "qemu.log").open("w") as log:
                completed = subprocess.run(command, stdout=log, stderr=log,
                                           env={**os.environ, "TMPDIR": str(ROOT / "out/tmp")}, timeout=300)
            text = serial.read_text(errors="replace")
            samples = [json.loads(row) for row in re.findall(r"VMXNET3_PERF (\{[^\r\n]+\})", text)]
            if completed.returncode or "VMXNET3_PERF_DONE:status=0" not in text or len(samples) != args.rounds:
                raise AssertionError("guest benchmark did not complete")
            if args.expect_msix and "irq=msix" not in text:
                raise AssertionError("MSI-X was not selected")
            if counts["tcp4"] != args.bytes * args.rounds or counts["udp4"] != 128 * args.rounds:
                raise AssertionError(f"echo counters differ: {counts}")
            for sample in samples:
                sample["echo_payload_mib_s"] = round(sample["bytes"] * 1e9 / sample["elapsed_ns"] / 1024 ** 2, 3)
                sample["guest_cpu_busy_pct"] = round(100 * sample["cpu_busy_ticks"] / max(1, sample["cpu_total_ticks"]), 2)
            result.update(passed=True, samples=samples, echoes=dict(counts))
        except Exception as error:
            result["error"] = repr(error)
            raise
        finally:
            result["echoes"] = dict(counts)
            (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
