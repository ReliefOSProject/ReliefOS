#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify VMXNET3 networking in a disposable, packaged ReliefOS QEMU guest."""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess
import time

from test_e1000e_qemu import echo_peers
from test_multi_nic_qemu import prepare

ROOT = Path(__file__).resolve().parents[1]


def guest_script(port, modular):
    load = "modprobe vmxnet3\n" if modular else ""
    lifecycle = f'''
rm -f /run/multinic-hold-ready
/probe eth0 10.0.2.2 fec0::2 {port} hold &
holder=$!
for attempt in $(seq 1 100); do
 test ! -f /run/multinic-hold-ready || break
 sleep 0.1
done
test -f /run/multinic-hold-ready
rmmod vmxnet3
if ifconfig eth1 >/dev/null 2>&1 || ifconfig eth2 >/dev/null 2>&1; then exit 1; fi
ping -c 1 -W 3 -I eth0 10.0.2.2
wait "$holder"
modprobe vmxnet3
configure eth3 10.0.3.2 fec1::15 fec1::2
configure eth4 10.0.4.2 fec2::15 fec2::2
/probe eth3 10.0.3.2 fec1::2 {port}
/probe eth4 10.0.4.2 fec2::2 {port}
echo VMXNET3_RELOAD_PASS
''' if modular else ""
    return f'''#!/bin/sh
export PATH=/bin
exec </dev/console >/dev/ttyS0 2>&1
finish() {{
 status=$?
 echo VMXNET3_QEMU_DONE:status=$status
 sync
 poweroff -f
 while :; do sleep 1; done
}}
trap finish EXIT
set -eu
test -c /dev/console
test -r /proc/net/dev
echo VMXNET3_QEMU_START
uname -a
{load}
configure() {{
 interface=$1
 udhcpc -f -n -q -i "$interface" -s /dhcp-script -t 4 -T 1
 ping -c 1 -W 3 -I "$interface" "$2"
 ip -6 addr add "$3/64" dev "$interface"
 attempts=0
 until ping6 -c 1 -W 1 -I "$interface" "$4"; do
  attempts=$((attempts + 1))
  test "$attempts" -lt 6
  sleep 1
 done
 echo VMXNET3_CONFIGURED:$interface
}}
configure eth0 10.0.2.2 fec0::15 fec0::2
configure eth1 10.0.3.2 fec1::15 fec1::2
configure eth2 10.0.4.2 fec2::15 fec2::2
cat /proc/net/dev /proc/net/if_inet6
/probe eth0 10.0.2.2 fec0::2 {port} &
first=$!
/probe eth1 10.0.3.2 fec1::2 {port} &
second=$!
/probe eth2 10.0.4.2 fec2::2 {port} &
third=$!
wait "$first"
wait "$second"
wait "$third"
echo VMXNET3_CONCURRENT_PASS
{lifecycle}
cat /proc/net/dev /proc/interrupts
ip addr show
echo VMXNET3_QEMU_PASS
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("install", "busybox", "probe", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--modular", action="store_true")
    parser.add_argument("--accel", choices=("kvm", "tcg"), default="kvm")
    parser.add_argument("--timeout", type=float, default=180)
    args = parser.parse_args()
    for name in ("install", "busybox", "probe", "output"):
        setattr(args, name, getattr(args, name).resolve())
    if args.output.exists() or not args.output.is_relative_to(ROOT / "out"):
        parser.error("output must be a new directory inside this checkout's out directory")
    manifest = json.loads((args.install / "install.bundle.json").read_text())
    expected = "m" if args.modular else "y"
    if manifest["configuration"].get("VMXNET3") != expected:
        parser.error(f"install configuration must have CONFIG_VMXNET3={expected}")
    if manifest["configuration"].get("E1000E") != "y":
        parser.error("install configuration must have CONFIG_E1000E=y")
    args.output.mkdir(parents=True)
    result = {"passed": False, "vmware_verified": False, "physical_hardware_verified": False}
    with echo_peers() as (port, counts):
        iso, manifest = prepare(args, port, script_factory=guest_script)
        serial = args.output / "serial.log"
        command = ["qemu-system-x86_64", "-machine", "q35", "-accel", args.accel,
                   "-cpu", "host" if args.accel == "kvm" else "max", "-m", "1024", "-smp", "2",
                   "-vga", "std", "-display", "none", "-serial", "file:" + str(serial),
                   "-cdrom", str(iso), "-boot", "d", "-nic", "none", "-no-reboot"]
        for index, model in enumerate(("e1000e", "vmxnet3", "vmxnet3")):
            network = (f"user,id=net{index},net=10.0.{index+2}.0/24,"
                       f"dhcpstart=10.0.{index+2}.15,ipv6-prefix=fec{index}::")
            command += ["-netdev", network, "-device", f"{model},netdev=net{index}"]
        result.update(command=command, release=manifest["release"],
                      kernel_sha256=manifest["files"]["kernel.sys"]["sha256"],
                      probe_sha256=hashlib.sha256(args.probe.read_bytes()).hexdigest(),
                      busybox_sha256=hashlib.sha256(args.busybox.read_bytes()).hexdigest())
        with (args.output / "qemu.log").open("w") as log:
            process = subprocess.Popen(command, stdout=log, stderr=log,
                                       env={**os.environ, "TMPDIR": str(ROOT / "out/tmp")})
            try:
                deadline = time.monotonic() + args.timeout
                while process.poll() is None and time.monotonic() < deadline:
                    time.sleep(.25)
                if process.poll() is None:
                    raise TimeoutError("VMXNET3 guest timed out")
                text = serial.read_text(errors="replace") if serial.exists() else ""
                markers = ["VMXNET3_CONCURRENT_PASS", "VMXNET3_QEMU_PASS", "VMXNET3_QEMU_DONE:status=0"]
                for interface in ("eth0", "eth1", "eth2"):
                    for family in (4, 6):
                        markers.append(f"MULTINIC_PASS device={interface} family={family}")
                if args.modular:
                    markers.append("VMXNET3_RELOAD_PASS")
                for marker in markers:
                    if marker not in text:
                        raise AssertionError("missing guest result " + marker)
                if "MULTINIC_FAIL" in text or "Kernel Panic" in text or process.returncode:
                    raise AssertionError("guest failed")
                if text.count("irq=msix") < 2:
                    raise AssertionError("both VMXNET3 controllers must negotiate MSI-X")
                messages = re.findall(r"(?m)^8[01]:\s+(\d+)\s+(\d+)\s+.*PCI-MSI", text)
                if len(messages) < 2 or any(sum(map(int, row)) == 0 for row in messages):
                    raise AssertionError("missing real MSI-X interrupts for both VMXNET3 controllers")
                expected_counts = ({"tcp4": 266240, "tcp6": 73728, "udp4": 24, "udp6": 24}
                                   if args.modular else
                                   {"tcp4": 36864, "tcp6": 36864, "udp4": 12, "udp6": 12})
                if any(counts[key] != value for key, value in expected_counts.items()):
                    raise AssertionError(f"host echo counts differ: {counts}")
                result.update(passed=True, returncode=process.returncode, echoes=dict(counts),
                              msix_messages=[sum(map(int, row)) for row in messages])
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
