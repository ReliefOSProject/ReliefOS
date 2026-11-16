#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify RTL8139 traffic, carrier and lifecycle in an owned ReliefOS QEMU guest."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import time

from test_e1000e_qemu import echo_peers
from test_multi_nic_qemu import prepare

ROOT = Path(__file__).resolve().parents[1]


def guest_script(port, modular):
    load = "modprobe rtl8139\n" if modular else ""
    lifecycle = f'''
rm -f /run/multinic-hold-ready
/probe eth0 10.0.2.2 fec0::2 {port} hold &
holder=$!
for attempt in $(seq 1 100); do
 test ! -f /run/multinic-hold-ready || break
 sleep 0.1
done
test -f /run/multinic-hold-ready
rmmod rtl8139
if ifconfig eth1 >/dev/null 2>&1 || ifconfig eth2 >/dev/null 2>&1; then exit 1; fi
if grep -q '^rtl8139 ' /proc/modules; then exit 1; fi
ping -c 1 -W 3 -I eth0 10.0.2.2
wait "$holder"
echo RTL8139_OTHER_OWNER_SURVIVED
modprobe rtl8139
configure eth3 10.0.3.2 fec1::15 fec1::2
configure eth4 10.0.4.2 fec2::15 fec2::2
/probe eth3 10.0.3.2 fec1::2 {port} stress
/probe eth4 10.0.4.2 fec2::2 {port} stress
echo RTL8139_RELOAD_PASS
''' if modular else ""
    return f'''#!/bin/sh
export PATH=/bin
exec </dev/console >/dev/ttyS0 2>&1
finish() {{
 status=$?
 echo RTL8139_QEMU_DONE:status=$status
 sync
 poweroff -f
 while :; do sleep 1; done
}}
trap finish EXIT
set -eu
echo RTL8139_QEMU_START
uname -a
{load}
configure() {{
 interface=$1
 udhcpc -f -n -q -i "$interface" -s /dhcp-script -t 5 -T 1
 ping -c 1 -W 3 -I "$interface" "$2"
 ip -6 addr add "$3/64" dev "$interface"
 attempts=0
 until ping6 -c 1 -W 1 -I "$interface" "$4"; do
  attempts=$((attempts + 1))
  test "$attempts" -lt 6
  sleep 1
 done
 echo RTL8139_CONFIGURED:$interface
}}
configure eth0 10.0.2.2 fec0::15 fec0::2
configure eth1 10.0.3.2 fec1::15 fec1::2
configure eth2 10.0.4.2 fec2::15 fec2::2
cat /proc/net/dev /proc/net/if_inet6
/probe eth0 10.0.2.2 fec0::2 {port} &
first=$!
/probe eth1 10.0.3.2 fec1::2 {port} stress &
second=$!
/probe eth2 10.0.4.2 fec2::2 {port} stress &
third=$!
wait "$first"
wait "$second"
wait "$third"
echo RTL8139_CONCURRENT_PASS
echo RTL8139_WAIT_LINK_DOWN
for attempt in $(seq 1 100); do
 if ifconfig eth1 | grep -q RUNNING; then sleep 0.1; else break; fi
done
if ifconfig eth1 | grep -q RUNNING; then exit 1; fi
ping -c 1 -W 3 -I eth2 10.0.4.2
echo RTL8139_LINK_DOWN_PASS
for attempt in $(seq 1 100); do
 if ifconfig eth1 | grep -q RUNNING; then break; else sleep 0.1; fi
done
ifconfig eth1 | grep -q RUNNING
ping -c 1 -W 3 -I eth1 10.0.3.2
echo RTL8139_LINK_UP_PASS
{lifecycle}
cat /proc/net/dev /proc/modules
ip addr show
echo RTL8139_QEMU_PASS
'''


def qmp_execute(path, command, arguments):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(5)
        client.connect(str(path))
        with client.makefile("rwb") as stream:
            greeting = json.loads(stream.readline())
            if "QMP" not in greeting:
                raise ValueError("QMP greeting missing")
            for name, params in (("qmp_capabilities", {}), (command, arguments)):
                stream.write(json.dumps({"execute": name, "arguments": params}).encode() + b"\n")
                stream.flush()
                while True:
                    response = json.loads(stream.readline())
                    if "error" in response:
                        raise RuntimeError(response["error"])
                    if "return" in response:
                        break


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("install", "busybox", "probe", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--modular", action="store_true")
    parser.add_argument("--accel", choices=("kvm", "tcg"), default="kvm")
    parser.add_argument("--firmware", choices=("bios", "uefi"), default="bios")
    parser.add_argument("--firmware-file", type=Path, default=Path("/usr/share/ovmf/OVMF.fd"))
    parser.add_argument("--timeout", type=float, default=360)
    args = parser.parse_args()
    for name in ("install", "busybox", "probe", "output"):
        setattr(args, name, getattr(args, name).resolve())
    if args.output.exists() or not args.output.is_relative_to(ROOT / "out"):
        parser.error("output must be a new directory inside this checkout's out directory")
    manifest = json.loads((args.install / "install.bundle.json").read_text())
    expected = "m" if args.modular else "y"
    if manifest["configuration"].get("RTL8139") != expected:
        parser.error(f"install configuration must have CONFIG_RTL8139={expected}")
    if manifest["configuration"].get("E1000E") != "y":
        parser.error("install configuration must have CONFIG_E1000E=y")
    args.output.mkdir(parents=True)
    result = {"passed": False, "vmware_verified": False, "physical_hardware_verified": False}
    with echo_peers() as (port, counts):
        iso, manifest = prepare(args, port, script_factory=guest_script)
        serial = args.output / "serial.log"
        qmp = args.output / "qmp.sock"
        command = ["qemu-system-x86_64", "-machine", "q35", "-accel", args.accel,
                   "-cpu", "host" if args.accel == "kvm" else "max", "-m", "1024", "-smp", "2",
                   "-vga", "std", "-display", "none", "-serial", "file:" + str(serial),
                   "-cdrom", str(iso), "-boot", "d", "-nic", "none", "-no-reboot",
                   "-qmp", f"unix:{qmp},server=on,wait=off"]
        if args.firmware == "uefi":
            command += ["-bios", str(args.firmware_file.resolve())]
        for index, model in enumerate(("e1000e", "rtl8139", "rtl8139")):
            network = (f"user,id=net{index},net=10.0.{index+2}.0/24,"
                       f"dhcpstart=10.0.{index+2}.15,ipv6-prefix=fec{index}::")
            command += ["-netdev", network, "-device", f"{model},id=nic{index},netdev=net{index}"]
        result.update(command=command, release=manifest["release"],
                      kernel_sha256=manifest["files"]["kernel.sys"]["sha256"],
                      probe_sha256=hashlib.sha256(args.probe.read_bytes()).hexdigest(),
                      busybox_sha256=hashlib.sha256(args.busybox.read_bytes()).hexdigest())
        with (args.output / "qemu.log").open("w") as log:
            process = subprocess.Popen(command, stdout=log, stderr=log,
                                       env={**os.environ, "TMPDIR": str(ROOT / "out/tmp")})
            try:
                deadline = time.monotonic() + args.timeout
                down = up = False
                while process.poll() is None and time.monotonic() < deadline:
                    text = serial.read_text(errors="replace") if serial.exists() else ""
                    if "RTL8139_WAIT_LINK_DOWN" in text and not down:
                        qmp_execute(qmp, "set_link", {"name": "nic1", "up": False})
                        down = True
                    if "RTL8139_LINK_DOWN_PASS" in text and not up:
                        qmp_execute(qmp, "set_link", {"name": "nic1", "up": True})
                        up = True
                    if "MULTINIC_FAIL" in text or "Kernel Panic" in text or "[FATAL]" in text:
                        raise AssertionError("guest reported a failure")
                    time.sleep(.2)
                if process.poll() is None:
                    raise TimeoutError("RTL8139 guest timed out")
                text = serial.read_text(errors="replace") if serial.exists() else ""
                markers = ["RTL8139_CONCURRENT_PASS", "RTL8139_LINK_DOWN_PASS", "RTL8139_LINK_UP_PASS",
                           "RTL8139_QEMU_PASS", "RTL8139_QEMU_DONE:status=0"]
                stress_devices = ["eth1", "eth2"]
                if args.modular:
                    markers += ["RTL8139_OTHER_OWNER_SURVIVED", "RTL8139_RELOAD_PASS"]
                    stress_devices += ["eth3", "eth4"]
                for interface in stress_devices:
                    for family in (4, 6):
                        markers.append(f"MULTINIC_STRESS device={interface} family={family} "
                                       "tcp_bytes=1048576 udp_datagrams=32")
                for marker in markers:
                    if marker not in text:
                        raise AssertionError("missing guest result " + marker)
                if process.returncode or text.count("irq=poll") < 2:
                    raise AssertionError("guest exited unsuccessfully or RTL8139 providers missing")
                expected_counts = {"tcp4": 1048576 * len(stress_devices) + 12288 + (204800 if args.modular else 0),
                                   "tcp6": 1048576 * len(stress_devices) + 12288 + (12288 if args.modular else 0),
                                   "udp4": 32 * len(stress_devices) + (8 if args.modular else 4),
                                   "udp6": 32 * len(stress_devices) + (8 if args.modular else 4)}
                if any(counts[key] != value for key, value in expected_counts.items()):
                    raise AssertionError(f"host echo counts differ: {counts}, expected {expected_counts}")
                if counts.get("client_errors") or counts.get("server_errors"):
                    raise AssertionError(f"host echo service failed: {counts}")
                result.update(passed=True, returncode=process.returncode, echoes=dict(counts),
                              carrier_down_up=down and up, modular=args.modular, firmware=args.firmware)
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
