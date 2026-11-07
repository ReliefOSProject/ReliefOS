#!/usr/bin/env python3
"""Run Ethernet ABI probes in a private, coherent packaged ReliefOS QEMU image.

The kernel owns the probe source. This script only stages real OS payloads,
provides loopback echo peers, and runs one disposable VM per invocation.
"""
import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import threading
import time
from types import SimpleNamespace
import uuid

import test_memory_qemu as image
import test_smp_latency_qemu as cohort
import test_vdso_qemu as disk

ROOT = Path(__file__).resolve().parents[1]


@contextmanager
def echo_peers():
    stop = threading.Event()
    counter_lock = threading.Lock()
    servers, threads, clients = [], [], []
    counts = {"tcp4": 0, "tcp6": 0, "udp4": 0, "udp6": 0}
    port = None

    def tcp_client(client, key):
        try:
            client.settimeout(.25)
            while not stop.is_set():
                try:
                    data = client.recv(65536)
                except socket.timeout:
                    continue
                if not data:
                    break
                client.sendall(data)
                with counter_lock:
                    counts[key] += len(data)
        except OSError:
            if not stop.is_set():
                with counter_lock:
                    counts.setdefault("client_errors", 0)
                    counts["client_errors"] += 1
        finally:
            client.close()

    def worker(server, kind, key):
        while not stop.is_set():
            try:
                if kind == socket.SOCK_DGRAM:
                    data, peer = server.recvfrom(65536)
                    server.sendto(data, peer)
                    with counter_lock:
                        counts[key] += 1
                else:
                    client, _ = server.accept()
                    clients.append(client)
                    thread = threading.Thread(target=tcp_client, args=(client, key))
                    threads.append(thread)
                    thread.start()
            except socket.timeout:
                pass
            except OSError:
                if not stop.is_set():
                    with counter_lock:
                        counts.setdefault("server_errors", 0)
                        counts["server_errors"] += 1
                break

    try:
        for family, host, suffix in ((socket.AF_INET, "127.0.0.1", "4"),
                                     (socket.AF_INET6, "::1", "6")):
            for kind, prefix in ((socket.SOCK_STREAM, "tcp"), (socket.SOCK_DGRAM, "udp")):
                server = socket.socket(family, kind)
                servers.append(server)
                if family == socket.AF_INET6:
                    server.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
                server.bind((host, port or 0))
                port = server.getsockname()[1]
                if kind == socket.SOCK_STREAM:
                    server.listen(8)
                server.settimeout(.25)
                thread = threading.Thread(target=worker, args=(server, kind, prefix + suffix))
                threads.append(thread)
                thread.start()
        yield port, counts
    finally:
        stop.set()
        for server in servers:
            server.close()
        for client in clients:
            try:
                client.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        # Listener shutdown precedes this snapshot, so no new clients are added.
        for thread in list(threads):
            thread.join(timeout=2)


def guest_script(port, modular):
    return f'''#!/bin/sh
set -eu
echo ETHERNET_QA_START
configure() {{
 mkdir -p /run/reliefos
 udhcpc -n -q -i eth0 -s /usr/share/udhcpc/default.script -t 5 -T 1
 cat /run/reliefos/dhcp-lease
 grep -q '^ip=10.0.2.15$' /run/reliefos/dhcp-lease
 echo ETHERNET_DHCP_OK
 ping -c 2 -W 3 10.0.2.2
 ip -6 addr add fec0::15/64 dev eth0
 sleep 2
 ping6 -c 2 -W 3 fec0::2
}}
configure
cat /proc/net/dev /proc/net/if_inet6
{image.GUEST}/lifecycle {port}
if test '{int(modular)}' = 1; then
 modinfo e1000e
 for cycle in 1 2; do
  rmmod e1000e
  if grep -q '^e1000e ' /proc/modules; then exit 1; fi
  if ifconfig eth0 >/dev/null 2>&1; then exit 1; fi
  modprobe e1000e
  grep '^e1000e ' /proc/modules
  configure
  {image.GUEST}/lifecycle {port}
  echo ETHERNET_RELOAD_OK:$cycle
 done
fi
echo ETHERNET_QA_PASS
'''


def boot(args, prepared):
    output, serial = args.output, args.output / "serial.log"
    cohort.verify_cohort_pins({"pins": prepared["cohort_pins"]})
    if cohort.sha256(Path(prepared["raw"])) != prepared["raw_sha256"]:
        raise ValueError("prepared image changed")
    directory_fd = os.open(ROOT / "out/tmp", os.O_RDONLY | os.O_DIRECTORY)
    qmp = Path(f"/proc/{os.getpid()}/fd/{directory_fd}/eth-{uuid.uuid4().hex[:10]}.sock")
    command = ["qemu-system-x86_64", "-machine", "q35", "-accel", args.accel,
               "-cpu", "host" if args.accel == "kvm" else "max", "-m", "1024", "-smp", "2",
               "-display", "none", "-serial", f"file:{serial}", "-no-reboot", "-no-shutdown",
               "-qmp", f"unix:{qmp},server=on,wait=off", "-nic", "none",
               "-netdev", "user,id=net0", "-device", f"{args.nic},netdev=net0",
               "-drive", f"file={prepared['raw']},format=raw,if=none,id=disk0,snapshot=on",
               "-device", "ide-hd,drive=disk0,bus=ide.0,bootindex=1"]
    if args.firmware == "uefi":
        command += ["-bios", str(args.firmware_file)]
    result = {"passed": False, "command": command, "vmware_verified": False,
              "hardware_verified": False, "release": prepared["release"]}
    process = None
    try:
        with (output / "qemu.log").open("w") as log:
            process = subprocess.Popen(command, cwd=ROOT, stdout=log, stderr=log,
                                       env={**os.environ, "TMPDIR": str(ROOT / "out/tmp")})
            result["pid"] = process.pid
            deadline = time.monotonic() + args.timeout
            while time.monotonic() < deadline:
                text = serial.read_text(errors="replace") if serial.exists() else ""
                if image.FATAL.search(text) or "ETHERNET_FAIL" in text:
                    raise RuntimeError("guest reported panic or Ethernet failure")
                if re.search(r"(?m)^MEMORY_RUN_DONE nonce=" + prepared["nonce"] + r" status=\d+\r?$", text):
                    image.parse_serial(text, prepared["nonce"], prepared["release"])
                    text = text.replace("\r", "")
                    copies = 3 if args.modular else 1
                    for family in (4, 6):
                        marker = f"ETHERNET_PASS family={family} tcp_streams=2 tcp_bytes=1048576 udp_datagrams=32"
                        if text.count(marker) != copies:
                            raise AssertionError("missing echo integrity results: " + marker)
                    if text.count("ETHERNET_DHCP_OK") != copies or text.count("ETHERNET_QA_PASS") != 1:
                        raise AssertionError("missing DHCP or completion marker")
                    if args.modular and (text.count("ETHERNET_RELOAD_OK:1") != 1 or
                                         text.count("ETHERNET_RELOAD_OK:2") != 1):
                        raise AssertionError("missing module reload results")
                    provider = "e1000e" if args.nic == "e1000e" else "e1000"
                    if f"[driver] loaded {provider} abi=2" not in text:
                        raise AssertionError("expected native provider was not loaded")
                    result.update(passed=True, ipv4=True, ipv6=True, dhcp_cycles=copies,
                                  tcp_payload_bytes=2 * 1048576 * copies,
                                  udp_datagrams=64 * copies, reload_cycles=2 if args.modular else 0)
                    break
                if process.poll() is not None:
                    raise RuntimeError("QEMU exited before guest completion")
                time.sleep(.2)
            else:
                raise TimeoutError("Ethernet guest timed out")
    except Exception as error:
        result["error"] = repr(error)
        raise
    finally:
        try:
            if process is not None:
                cohort.stop_owned(process, qmp)
                result["qemu_exit"] = process.returncode
            result["cleanup"] = "ok"
            if (ROOT / "out/tmp" / qmp.name).exists():
                (ROOT / "out/tmp" / qmp.name).unlink()
        except Exception as error:
            result["passed"] = False
            result["cleanup"] = repr(error)
            raise
        finally:
            os.close(directory_fd)
            result["source_unchanged"] = cohort.sha256(args.image) == prepared["source_sha256"]
            result["prepared_unchanged"] = cohort.sha256(Path(prepared["raw"])) == prepared["raw_sha256"]
            if not result["source_unchanged"] or not result["prepared_unchanged"]:
                result["passed"] = False
                result["error"] = "input image was modified"
            (output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    if not result["source_unchanged"] or not result["prepared_unchanged"]:
        raise RuntimeError("input image was modified")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--root-payload", type=Path, required=True)
    parser.add_argument("--boot-payload", type=Path, required=True)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--nic", choices=("e1000e", "e1000", "e1000-82545em"), default="e1000e")
    parser.add_argument("--modular", action="store_true")
    parser.add_argument("--firmware", choices=("bios", "uefi"), default="bios")
    parser.add_argument("--firmware-file", type=Path, default=disk.OVMF)
    parser.add_argument("--accel", choices=("kvm", "tcg"), default="kvm")
    parser.add_argument("--timeout", type=float, default=180)
    args = parser.parse_args()
    for name in ("image", "root_payload", "boot_payload", "kernel", "sdk", "output"):
        setattr(args, name, getattr(args, name).resolve())
    if not args.output.is_relative_to(ROOT / "out") or args.output.exists():
        parser.error("output must be a new directory inside this checkout out/")
    if args.modular and args.nic != "e1000e":
        parser.error("module reload case requires e1000e")
    cohort.refuse_existing_vm()
    args.output.mkdir(parents=True)
    probe = args.output / "ethernet-probe"
    subprocess.run([str(args.sdk / "bin/reliefos-musl-cc"), "-static", "-O2", "-std=c11",
                    "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare",
                    str(ROOT / "kernel/reliefnt/tools/tests/ethernet_guest.c"), "-o", str(probe)], check=True)
    with echo_peers() as (port, counts):
        workload = args.output / "workload.sh"
        workload.write_text(guest_script(port, args.modular))
        parameters = SimpleNamespace(output=args.output, source=args.image,
                                     source_sha256=cohort.sha256(args.image),
                                     root_payload=args.root_payload, boot_payload=args.boot_payload,
                                     kernel=args.kernel, lifecycle=probe, pressure=probe, oom=probe,
                                     workload=workload)
        prepared = image.prepare(parameters)
        result = boot(args, prepared)
        copies = 3 if args.modular else 1
        if any(counts[f"tcp{v}"] != 1048576 * copies or counts[f"udp{v}"] != 32 * copies
               for v in (4, 6)) or "server_errors" in counts or "client_errors" in counts:
            result["passed"] = False
            result["error"] = "host echo counters differ from guest results"
        result["peer_counters"] = counts.copy()
        (args.output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
        if not result["passed"]:
            raise RuntimeError(result)
        print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
