#!/usr/bin/env python3
"""Boot a copy of the production root and report real upstream guest behavior.

Uses the existing inventory autospawn slot only in this disposable test image;
no kernel changes, production tool substitutions, or host block-device access.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time
import test_linux_ioctl_cloexec as iso_tools
from test_power_qemu import PowerProbe

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / "build/storage-upstream-guest"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--filesystem", choices=("ext2", "ext4"), default="ext4")
    parser.add_argument("--timeout", type=int, default=360)
    parser.add_argument("--root", type=Path)
    parser.add_argument("--power", choices=("reboot", "poweroff"))
    parser.add_argument("--smp", type=int, choices=(1,2), default=2)
    args = parser.parse_args()
    if args.root is None:
        args.root = ROOT / "out/x86_64/release/images/disk-root.ext4" if args.filesystem == "ext4" else ROOT / "build/live/root.ext2"
    args.root = args.root.resolve()
    work = WORK / (args.root.parent.name + ("-" + args.power if args.power else "") + f"-smp{args.smp}")
    work.mkdir(parents=True, exist_ok=True)
    (work / "result.json").unlink(missing_ok=True)
    with args.root.open("rb") as source:
        base_hash = hashlib.file_digest(source, "sha256").hexdigest()
    compiler = ROOT / "build/musl-gcc/root/opt/dyne/gcc-musl/bin/x86_64-linux-musl-gcc"
    compiler_args = [str(compiler)]
    if not compiler.exists():
        compiler_args = ["clang", "--target=x86_64-linux-musl",
                         f"--sysroot={ROOT / 'out/x86_64/release/sysroot/musl'}",
                         "-fuse-ld=lld"]
    probe = work / "probe.elf"
    subprocess.run([*compiler_args, "-static", "-O2", "-Wall", "-Wextra",
                    *([f'-DPROBE_POWER_COMMAND="{args.power}"'] if args.power else []),
                    "tools/tests/storage_upstream_guest.c", "kernel/reliefnt/tools/tests/tmpfs_mmap_test.c",
                    "-o", str(probe)], cwd=ROOT, check=True)
    image = work / "root.ext2"
    shutil.copy2(args.root, image)
    target = "/usr/lib/reliefos/tests/linux-inventory.elf"
    # debugfs may return zero on command errors; verify bytes after injection.
    for command in ("mkdir /usr/lib/reliefos/tests", f"rm {target}", f"write {probe} {target}",
                    f"set_inode_field {target} mode 0100755"):
        subprocess.run(["debugfs", "-w", "-R", command, str(image)], check=True, capture_output=True)
    embedded = subprocess.check_output(["debugfs", "-R", f"cat {target}", str(image)], stderr=subprocess.DEVNULL)
    if embedded != probe.read_bytes():
        raise RuntimeError("guest probe was not embedded correctly")
    iso_tools.GRUB_TEMPLATE = iso_tools.GRUB_TEMPLATE.replace(
        "autospawn=ioctlcloexec autospawn=python315", "autospawn=inventory").replace(
        "syscall-trace=/opt/python/", "").replace("ioctl CLOEXEC regression", "official storage tools probe")
    iso = iso_tools.build_iso(image, work / "storage-test.iso", work / "grub.cfg", work)
    kernel_path = ROOT / "out/x86_64/release/generated/system/kernel.sys"
    kernel_hash = hashlib.sha256(kernel_path.read_bytes()).hexdigest()
    probe_hash = hashlib.sha256(probe.read_bytes()).hexdigest()
    serial = work / "serial.log"
    serial.write_text("")
    disk=work / "disposable.raw"
    with disk.open("wb") as output:
        output.truncate(64 << 20)
    subprocess.run(["sfdisk",str(disk)],input="label: gpt\nstart=2048,size=32768,type=L\n",
                   text=True,check=True,capture_output=True)
    with tempfile.TemporaryDirectory(prefix="storage-qmp-") as directory, (work / "qemu.log").open("w") as errors:
        qmp = Path(directory) / "qmp.sock"
        command = ["qemu-system-x86_64", "-enable-kvm", "-cpu", "host", "-machine", "q35", "-m", "4096",
                   "-smp", str(args.smp), "-bios", "/usr/share/edk2/x64/OVMF.4m.fd", "-display", "none",
                   "-serial", f"file:{serial}", "-device", "VGA,xres=1280,yres=720",
                   "-cdrom", str(iso), "-boot", "d", "-qmp", f"unix:{qmp},server=on,wait=off",
                   "-drive",f"file={disk},format=raw,if=none,id=testdisk",
                   "-device","ide-hd,drive=testdisk,bus=ide.1",
                   "-no-reboot", "-no-shutdown"]
        process = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.DEVNULL, stderr=errors)
        deadline = time.monotonic() + args.timeout
        power_event = None
        validation_error = None
        try:
            if args.power:
                while not qmp.exists() and time.monotonic() < deadline:
                    if process.poll() is not None:
                        raise RuntimeError("QEMU exited before QMP was ready")
                    time.sleep(.1)
                probe = PowerProbe(qmp, work)
                try:
                    request = f"[storage-upstream] upstream BusyBox {args.power} request"
                    while request not in serial.read_text(errors="replace"):
                        if time.monotonic() >= deadline or process.poll() is not None:
                            raise RuntimeError("guest did not reach the BusyBox power request")
                        if "[storage-upstream] FAIL" in serial.read_text(errors="replace"):
                            raise RuntimeError("guest could not prepare the BusyBox power request")
                        time.sleep(.1)
                    power_event = probe.expect_transition("SHUTDOWN")
                finally:
                    probe.close()
            else:
                while time.monotonic() < deadline:
                    text = serial.read_text(errors="replace")
                    if "[storage-upstream] DONE" in text or "KERNEL PANIC" in text or process.poll() is not None:
                        break
                    time.sleep(.5)
        except (AssertionError, RuntimeError, EOFError, OSError) as error:
            validation_error = str(error)
        finally:
            if process.poll() is None:
                iso_tools.qmp_quit(qmp, process)
    text = serial.read_text(errors="replace")
    passed = re.findall(r"\[storage-upstream\] PASS ([^\r\n]+)", text)
    failed = re.findall(r"\[storage-upstream\] FAIL ([^\r\n]+)", text)
    complete = not validation_error and "[storage-upstream] DONE failures=0" in text
    if args.power:
        magic = "1234567" if args.power == "reboot" else "4321fedc"
        reason = "guest-reset" if args.power == "reboot" else "guest-shutdown"
        complete = bool(not validation_error and power_event and power_event["data"].get("reason") == reason and
                        f"reboot(2) requested by pid=1 command=0x{magic}" in text)
    evidence = {"base_root": str(args.root), "base_sha256": base_hash,
                "kernel_sha256": kernel_hash, "probe_sha256": probe_hash,
                "qemu_command": command, "passed": passed,
                "failed": failed, "complete": complete, "serial_log": str(serial),
                "power_event": power_event, "validation_error": validation_error}
    (work / "result.json").write_text(json.dumps(evidence, indent=2) + "\n")
    print(json.dumps(evidence, indent=2))
    if not complete:
        raise SystemExit("guest validation did not pass; see serial.log and result.json")


if __name__ == "__main__":
    main()
