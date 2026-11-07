#!/usr/bin/env python3
"""Build and run the native musl ABI probes on the pinned Linux 6.12 kernel."""
import argparse
import hashlib
import re
import shutil
from pathlib import Path
import socket
import subprocess
import tempfile
import time

from build_musl_ltp import ROOT, SOURCES


def run(command, **kwargs):
    subprocess.run([str(value) for value in command], check=True, cwd=ROOT, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--proxy", help="override the proxy from the environment (optional)")
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--probe-source", type=Path, default=ROOT / "kernel/reliefnt/tools/tests/musl_guest_test.c")
    parser.add_argument("--probe-binary", type=Path)
    parser.add_argument("--file", nargs=2, action="append", default=[], metavar=("SOURCE", "GUEST_PATH"))
    parser.add_argument("--case", help="Run only the named probe case")
    parser.add_argument("--log", type=Path, default=ROOT / "build/musl/linux-reference-serial.log")
    args = parser.parse_args()
    cache = ROOT / "build"
    source = cache / "linux-reference-source"
    output = cache / "linux-reference"
    sdk = cache / "musl/sdk"
    output.mkdir(parents=True, exist_ok=True)
    filename, url, digest, _ = next(item for item in SOURCES if item[0] == "linux-6.12.tar.xz")
    archive = cache / filename
    if not archive.exists():
        run(["curl", "--fail", "--location",
             *(["--proxy", args.proxy] if args.proxy is not None else []), url, "-o", archive])
    with archive.open("rb") as stream:
        if hashlib.file_digest(stream, "sha256").hexdigest() != digest:
            raise SystemExit("Linux source archive checksum mismatch")
    if not source.exists():
        source.mkdir()
        run(["tar", "-xf", archive, "-C", source, "--strip-components=1"])
    make = ["make", "-C", source, f"O={output}", "ARCH=x86_64",
            "CC=gcc -std=gnu11", "HOSTCC=gcc -std=gnu11"]
    build_log = output / "reference-build.log"
    with build_log.open("w") as log:
        if not (output / ".config").exists():
            run([*make, "tinyconfig"], stdout=log, stderr=subprocess.STDOUT)
        options = ("64BIT X86_64 PRINTK BUG ELF_CORE BINFMT_ELF BINFMT_SCRIPT MULTIUSER FUTEX POSIX_TIMERS "
                   "HIGH_RES_TIMERS TTY SERIAL_8250 SERIAL_8250_CONSOLE BLK_DEV_INITRD RD_GZIP "
                   "DEVTMPFS TMPFS SHMEM PROC_FS SYSFS NET UNIX UNIX98_PTYS EPOLL EVENTFD "
                   "SIGNALFD TIMERFD FILE_LOCKING ADVISE_SYSCALLS MEMBARRIER X86_LOCAL_APIC SMP "
                   "HYPERVISOR_GUEST PARAVIRT KVM_GUEST CROSS_MEMORY_ATTACH ACPI").split()
        run([source / "scripts/config", "--file", output / ".config",
             *(argument for name in options for argument in ("-e", name)), "-d", "WERROR"],
            stdout=log, stderr=subprocess.STDOUT)
        run([*make, "olddefconfig"], stdout=log, stderr=subprocess.STDOUT)
        run([*make, f"-j{args.jobs}", "bzImage", "headers_install", f"INSTALL_HDR_PATH={output / 'headers'}"],
            stdout=log, stderr=subprocess.STDOUT)
    compiler = sdk / "bin/reliefos-musl-cc"
    run([compiler, "-static", "-O2", *([f'-DREFERENCE_CASE="{args.case}"'] if args.case else []),
         ROOT / "kernel/reliefnt/tools/tests/linux_reference_init.c", "-o", output / "reference-init"])
    if args.probe_binary:
        shutil.copy2(args.probe_binary.resolve(), output / "probe")
    else:
        run([compiler, "-static", "-O2", "-pthread", '-DPROBE_KIND="static"',
             "-I", output / "headers/include", args.probe_source.resolve(), "-o", output / "probe"])
    archive_list = (ROOT / "tools/tests/linux-reference-initramfs.list").read_text()
    directories = {line.split()[1] for line in archive_list.splitlines() if line.startswith("dir ")}
    for original, guest in args.file:
        original = Path(original).resolve()
        path = Path(guest)
        if not path.is_absolute() or ".." in path.parts or any(c.isspace() for c in str(original) + guest):
            parser.error(f"unsafe initramfs destination or unsupported whitespace: {guest}")
        for parent in reversed(path.parents):
            if str(parent) != "/" and str(parent) not in directories:
                archive_list += f"dir {parent} 0755 0 0\n"
                directories.add(str(parent))
        archive_list += f"file {guest} {original} {original.stat().st_mode & 0o7777:04o} 0 0\n"
    (output / "probe-initramfs.list").write_text(archive_list)
    with (output / "tests.cpio").open("wb") as initramfs:
        run([output / "usr/gen_init_cpio", output / "probe-initramfs.list"], stdout=initramfs)
    serial = args.log.resolve()
    serial.parent.mkdir(parents=True, exist_ok=True)
    serial.write_text("")
    with tempfile.TemporaryDirectory(prefix="reliefos-linux-reference-") as directory:
        qmp = Path(directory) / "qmp.sock"
        command = ["qemu-system-x86_64", "-enable-kvm", "-cpu", "host", "-machine", "q35",
                   "-m", "512", "-smp", "2", "-kernel", str(output / "arch/x86/boot/bzImage"),
                   "-initrd", str(output / "tests.cpio"), "-append",
                   "console=ttyS0 rdinit=/init panic=-1 random.trust_cpu=on", "-display", "none",
                   "-serial", f"file:{serial}", "-qmp", f"unix:{qmp},server=on,wait=off",
                   "-no-reboot", "-no-shutdown"]
        process = subprocess.Popen(command, cwd=ROOT)
        text = ""
        try:
            deadline = time.monotonic() + args.timeout
            while process.poll() is None and time.monotonic() < deadline:
                if serial.exists():
                    text = serial.read_text(errors="replace")
                    if re.search(r"\[linux-reference\] DONE failures=\d+", text):
                        break
                time.sleep(0.1)
        finally:
            if process.poll() is None:
                try:
                    with socket.socket(socket.AF_UNIX) as connection:
                        connection.settimeout(2)
                        connection.connect(str(qmp))
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
                        process.wait()
        print("\n".join(line for line in text.splitlines() if "[linux-reference]" in line), flush=True)
        if (not re.search(r"^\[linux-reference\] kernel=6\.12\.0$", text, re.M) or
                "[linux-reference] online_cpus=2" not in text or
                "[linux-reference] DONE failures=0" not in text):
            raise SystemExit(f"Linux reference probe failed or timed out; see {serial}")
    print(f"PASS Linux 6.12 reference; serial={serial}; build={build_log}")


if __name__ == "__main__":
    main()
