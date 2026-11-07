#!/usr/bin/env python3
"""Boot build/gcc-probe/gcc-probe.vmdk under QEMU/KVM and collect the guest serial log.

The image is produced by::

    LEONOS_GCC_ARCHIVE=/path/to/dyne-gcc-musl-x86_64.tar.xz \
        python3 build.py run gcc-probe-image

This script only starts the already built image in snapshot mode; it never
modifies the prebuilt GCC/musl binaries or the disk image.
"""
import argparse
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def qmp_quit(sock_path: Path, process: subprocess.Popen) -> None:
    try:
        with socket.socket(socket.AF_UNIX) as connection:
            connection.settimeout(2)
            connection.connect(str(sock_path))
            connection.recv(65536)
            connection.sendall(b'{"execute":"qmp_capabilities"}\n')
            connection.recv(65536)
            connection.sendall(b'{"execute":"quit"}\n')
        process.wait(timeout=5)
    except (OSError, subprocess.TimeoutExpired):
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--smp", type=int, default=1)
    parser.add_argument("--memory-mib", type=int, default=4096)
    parser.add_argument("--timeout", type=float, default=900.0)
    parser.add_argument("--serial", type=Path,
                        default=ROOT / "build/gcc-probe/guest-serial.log")
    parser.add_argument("--qmp", type=Path, default=ROOT / "build/gcc-probe/qmp.sock")
    args = parser.parse_args()
    image = ROOT / "build/gcc-probe/gcc-probe.vmdk"
    if not image.exists():
        parser.error(f"missing {image}; run build.py run gcc-probe-image first")
    args.serial.parent.mkdir(parents=True, exist_ok=True)
    args.serial.write_text("")
    try:
        args.qmp.unlink()
    except FileNotFoundError:
        pass
    command = [
        "qemu-system-x86_64", "-enable-kvm", "-cpu", "host", "-machine", "q35",
        "-m", f"{args.memory_mib}M", "-smp", str(args.smp),
        "-bios", "/usr/share/edk2/x64/OVMF.4m.fd", "-display", "none",
        "-serial", f"file:{args.serial}",
        "-device", "VGA,xres=1280,yres=720",
        "-netdev", "user,id=net0", "-device", "e1000,netdev=net0",
        "-drive", f"file={image},if=none,id=sata0,format=vmdk,snapshot=on",
        "-device", "ich9-ahci,id=ahci", "-device", "ide-hd,drive=sata0,bus=ahci.0",
        "-qmp", f"unix:{args.qmp},server=on,wait=off",
        "-no-reboot", "-no-shutdown",
    ]
    process = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
    deadline = time.monotonic() + args.timeout
    text = ""
    try:
        while process.poll() is None and time.monotonic() < deadline:
            time.sleep(0.2)
            if args.serial.exists():
                text = args.serial.read_text(errors="replace")
                if "[gcc-probe] DONE failures=" in text:
                    time.sleep(1.0)
                    text = args.serial.read_text(errors="replace")
                    break
    finally:
        if process.poll() is None:
            qmp_quit(args.qmp, process)
    key_lines = [line for line in text.splitlines()
                 if "[vfork-stack]" in line or "[vfork-edge]" in line or "[gcc-probe]" in line]
    print("\n".join(key_lines), flush=True)
    if "[gcc-probe] DONE failures=0" not in text:
        print(f"QEMU probe failed or timed out; serial={args.serial}", file=sys.stderr)
        return 1
    print(f"PASS gcc-probe QEMU smp={args.smp}; serial={args.serial}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
