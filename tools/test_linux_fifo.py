#!/usr/bin/env python3
"""Run identical static-musl FIFO checks on Linux and optionally ReliefNT.

--guest copies the built production staging tree into a disposable probe image,
updates its kernel from build/system, and runs the probe before the real init.
It never modifies the production image. OpenRC boot remains a separate check.
"""
import argparse
import json
import socket
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import struct
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from run_gcc_probe_qemu import qmp_quit


def run(*args, **kwargs):
    return subprocess.run([str(a) for a in args], cwd=ROOT, check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--guest", action="store_true")
    parser.add_argument("--smp", type=int, default=2)
    parser.add_argument("--ntp-probe", action="store_true", help="Use a local protocol-correct NTP responder; guest only")
    args = parser.parse_args()
    work = ROOT / "build/openrc-fifo-probe"
    work.mkdir(parents=True, exist_ok=True)
    binary = work / "fifo-abi"
    run(ROOT / "build/musl/sdk/bin/reliefos-musl-cc", "-static", "-O2", "-Wall", "-Wextra", "-Werror",
        ROOT / "tools/tests/fifo_abi_test.c", "-o", binary)
    host = run(binary, capture_output=True, text=True, timeout=15)
    (work / "host.log").write_text(host.stdout + host.stderr)
    print(host.stdout, end="")
    if not args.guest:
        return
    ntp = None
    requests = []
    if args.ntp_probe:
        ntp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        ntp.bind(("127.0.0.1", 0))
        ntp.settimeout(0.5)
        def timestamp():
            value = time.time() + 2208988800 + 120
            seconds = int(value)
            return struct.pack("!II", seconds, int((value-seconds) * 2**32))
        def serve():
            while ntp.fileno() >= 0:
                try:
                    packet, peer = ntp.recvfrom(2048)
                    if len(packet) < 48: continue
                    requests.append(time.time())
                    reply = bytearray(48)
                    reply[:4] = bytes([0x24, 2, 4, 236])
                    reply[8:12] = struct.pack("!I", 66)
                    reply[12:16] = b"LOCL"
                    reply[16:24] = timestamp()
                    reply[24:32] = packet[40:48]
                    reply[32:40] = timestamp()
                    reply[40:48] = timestamp()
                    ntp.sendto(reply, peer)
                except socket.timeout: continue
                except OSError: break
        threading.Thread(target=serve, daemon=True).start()
    with tempfile.TemporaryDirectory(prefix="reliefos-fifo-stage-") as temporary:
        tree = Path(temporary) / "esp"
        run("cp", "-a", "--reflink=auto", ROOT / "build/apk/root", tree)
        shutil.copy2(ROOT / "build/system/kernel.sys", tree / "leonos/kernel.sys")
        shutil.copy2(binary, tree / "usr/bin/fifo-abi")
        shutil.copy2(ROOT / "build/userland/busybox.elf", tree / "bin/busybox")
        shutil.copytree(ROOT / "system/rootfs/usr/share/udhcpc", tree / "usr/share/udhcpc", dirs_exist_ok=True)
        shutil.copy2(ROOT / "system/rootfs/usr/lib/reliefos/ntp-status", tree / "usr/lib/reliefos/ntp-status")
        shutil.copytree(ROOT / "system/rootfs/etc/init.d", tree / "etc/init.d", dirs_exist_ok=True)
        if ntp:
            (tree / "etc/conf.d").mkdir(exist_ok=True)
            (tree / "etc/conf.d/reliefos-ntp").write_text(
                f'command_args="-n -dddd -p 10.0.2.2:{ntp.getsockname()[1]}"\n')
        report = tree / "usr/bin/openrc-probe-report"
        report.write_text("#!/bin/sh\nsleep 20\necho '[openrc-probe] service logs'\n"
            "for f in /var/log/imd.log /var/log/windowd.log /var/log/desktop.log /var/log/sessiond.log /var/log/device-agent.log /var/log/udhcpc.log /var/log/ntpd.log; do echo \"$f\"; cat \"$f\"; done\n"
            "cat /run/reliefos/dhcp-lease /run/reliefos/ntp-state /etc/resolv.conf\n/bin/busybox nslookup pool.ntp.org\n/bin/rc-status -a\necho '[openrc-probe] report end'\n")
        report.chmod(0o755)
        inittab = tree / "etc/inittab"
        inittab.write_text(inittab.read_text() + "\n::once:/usr/bin/openrc-probe-report\n")
        grub = tree / "grub/grub.cfg"
        text = grub.read_text()
        text = text.replace("set timeout=5", "set timeout=0", 1)
        text = text.replace("multiboot2 /reliefos/kernel.sys root=/", "multiboot2 /reliefos/kernel.sys init=/usr/bin/fifo-abi syscall-trace=/usr/bin/fifo-abi root=/", 1)
        grub.write_text(text)
        with (work / "image.log").open("w") as log:
            run(sys.executable, "tools/make_image.py", "--esp-tree", tree,
                "--out", work / "probe.vmdk", "--raw", work / "probe.raw",
                "--esp-image", work / "esp.fat", "--root-image", work / "root.ext2",
                stdout=log, stderr=subprocess.STDOUT)
    serial = work / f"guest-smp{args.smp}.log"
    serial.write_text("")
    with tempfile.TemporaryDirectory(prefix="reliefos-fifo-qmp-") as temporary:
        qmp = Path(temporary) / "qmp.sock"
        command = ["qemu-system-x86_64", "-enable-kvm", "-cpu", "host", "-machine", "q35",
            "-m", "4096", "-smp", str(args.smp), "-bios", "/usr/share/edk2/x64/OVMF.4m.fd",
            "-display", "none", "-serial", f"file:{serial}", "-device", "VGA,xres=1280,yres=720",
            "-netdev", "user,id=net0", "-device", "e1000,netdev=net0",
            "-drive", f"file={work / 'probe.vmdk'},if=none,id=sata0,format=vmdk,snapshot=on",
            "-device", "ich9-ahci,id=ahci", "-device", "ide-hd,drive=sata0,bus=ahci.0",
            "-qmp", f"unix:{qmp},server=on,wait=off", "-no-reboot", "-no-shutdown"]
        with (work / "qemu.log").open("w") as log:
            process = subprocess.Popen(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 60
            completed = None
            try:
                while process.poll() is None and time.monotonic() < deadline:
                    text = serial.read_text(errors="replace")
                    if "[fifo] DONE" in text:
                        if completed is None:
                            completed = time.monotonic()
                        if time.monotonic() - completed > 40:
                            break
                    time.sleep(0.2)
            finally:
                if process.poll() is None:
                    try:
                        with socket.socket(socket.AF_UNIX) as connection:
                            connection.settimeout(3)
                            connection.connect(str(qmp))
                            connection.recv(4096)
                            connection.sendall(b'{"execute":"qmp_capabilities"}\n')
                            connection.recv(4096)
                            connection.sendall((json.dumps({"execute": "screendump", "arguments": {
                                "filename": str(work / "desktop.ppm")}}) + "\n").encode())
                            connection.recv(4096)
                    finally:
                        qmp_quit(qmp, process)
    if ntp:
        ntp.close()
        (work / "ntp-responder.log").write_text(f"requests={len(requests)}\n")
    text = serial.read_text(errors="replace")
    print("\n".join(line for line in text.splitlines() if "[fifo]" in line))
    if "[fifo] DONE failures=0" not in text or "[fifo] FAIL" in text:
        raise SystemExit(f"FAIL guest FIFO probe; evidence: {serial}")
    print(f"PASS FIFO on ReliefNT smp={args.smp}; OpenRC boot is not certified by this test")


if __name__ == "__main__":
    main()
