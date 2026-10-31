#!/usr/bin/env python3
"""Boot real PAM DSOs on an exclusive ext2 diagnostic disk, never a user disk."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import time

from run_gcc_probe_qemu import qmp_quit
from test_auth_upstream import configure_tests, account_fixture

ROOT = Path(__file__).resolve().parents[1]


def overlay_auth_tree(source: Path, destination: Path, stage: Path):
    """Replace leaves without following aliases from the normal rootfs."""
    if destination.is_symlink() or not destination.resolve().is_relative_to(stage):
        raise ValueError(f"unsafe staging directory: {destination}")
    destination.mkdir(parents=True, exist_ok=True)
    for entry in source.iterdir():
        target = destination / entry.name
        if entry.is_dir() and not entry.is_symlink():
            overlay_auth_tree(entry, target, stage)
        else:
            if target.is_symlink() or target.is_file():
                target.unlink()
            if entry.is_symlink():
                target.symlink_to(os.readlink(entry))
            else:
                shutil.copy2(entry, target)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--smp", type=int, default=1)
    parser.add_argument("--require-smp", action="store_true",
                        help="Require the requested CPUs to be online, not only configured in QEMU")
    parser.add_argument("--probe", type=Path, default=ROOT / "build/auth-upstream/pam-runtime.elf")
    parser.add_argument("--marker", default="pam-runtime")
    parser.add_argument("--unix", action="store_true", help="Exercise pam_unix with scratch accounts")
    parser.add_argument("--sudo", action="store_true", help="Stage official sudo and su commands in the diagnostic disk")
    parser.add_argument("--ltp", action="store_true", help="Stage the repository's pinned musl LTP subset")
    parser.add_argument("--file", nargs=2, action="append", default=[], metavar=("SOURCE", "GUEST_PATH"))
    parser.add_argument("--trace-storage", action="store_true")
    args = parser.parse_args()
    output = args.output.resolve()
    if not output.is_relative_to(ROOT / "build") or output.exists():
        parser.error("--output must be a new directory below build/")
    output.mkdir(parents=True)
    temporary = output / "tmp"
    temporary.mkdir()
    stage = output / "stage"
    subprocess.run(["cp", "--reflink=auto", "-a", str(ROOT / "build/esp"), str(stage)], check=True)
    for source, destination in (("build/system/kernel.sys", "reliefos/kernel.sys"),
                                ("build/system/kernel.sys", "leonos/kernel.sys")):
        shutil.copy2(ROOT / source, stage / destination)
    auth = ROOT / "build/auth-upstream"
    overlay_auth_tree(auth / "root/lib", stage / "lib", stage)
    for name in ("libc.so", "libmimalloc.so.3"):
        shutil.copy2(ROOT / "build/musl/sysroot/lib" / name, stage / "lib" / name)
    overlay_auth_tree(auth / "root/sbin", stage / "sbin", stage)
    if args.sudo:
        overlay_auth_tree(auth / "root/usr", stage / "usr", stage)
        overlay_auth_tree(auth / "root/bin", stage / "bin", stage)
    tests = stage / "usr/lib/reliefos/tests"
    tests.mkdir(parents=True, exist_ok=True)
    if args.ltp:
        shutil.copytree(ROOT / "build/musl/ltp", tests, dirs_exist_ok=True)
    # Reuse the existing diagnostic executable slot in this isolated image.
    # The installed production gcc probe and normal command entries are untouched.
    shutil.copy2(args.probe.resolve(), tests / "gcc-probe.elf")
    extra_files = []
    for source, guest in args.file:
        path = Path(guest)
        destination = stage / str(path).lstrip("/")
        if not path.is_absolute() or ".." in path.parts or not destination.resolve().is_relative_to(stage):
            parser.error(f"unsafe guest destination: {guest}")
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(Path(source).resolve(), destination)
        extra_files.append(destination)
    configure_tests(tests / "pam.d", "/lib/security")
    if args.unix:
        account_fixture(tests / "pam-fixture")
    (stage / "grub/grub.cfg").write_text('''set timeout=0
set default=0
insmod part_gpt
insmod fat
insmod multiboot2
insmod all_video
set gfxpayload=keep
menuentry "ReliefOS PAM runtime probe" {
    set root=(hd0,gpt1)
    multiboot2 /reliefos/kernel.sys root=/ log=serial autospawn=gcc
    boot
}
''')
    hashes = {}
    artifacts = sorted((stage / "lib").glob("libpam*.so*")) + sorted((stage / "lib/security").glob("*.so"))
    artifacts += sorted((stage / "lib").glob("libcrypt.so*"))
    artifacts += sorted((stage / "lib").glob("libbsd.so*"))
    artifacts += sorted((stage / "lib").glob("libmd.so*"))
    artifacts += [stage / "sbin" / helper.name for helper in sorted((auth / "root/sbin").iterdir())
                  if helper.is_file()]
    artifacts += [tests / "gcc-probe.elf", stage / "reliefos/kernel.sys",
                  stage / "leonos/kernel.sys", stage / "lib/ld-musl-x86_64.so.1"]
    artifacts += [stage / "usr/lib/leonos/libleonos.so.2",
                  stage / "usr/lib/reliefos/apps/authd/authd.elf"]
    artifacts += extra_files
    if args.sudo:
        artifacts += [stage / "usr/bin/sudo", stage / "usr/sbin/visudo", stage / "bin/su",
                      stage / "usr/bin/passwd"]
        artifacts += sorted((stage / "usr/lib/sudo").glob("*.so"))
    if args.ltp:
        artifacts += sorted(tests.glob("*.elf"))
    for path in artifacts:
        with path.open("rb") as stream:
            hashes["/" + str(path.relative_to(stage))] = hashlib.file_digest(stream, "sha256").hexdigest()
    (output / "pam-files.json").write_text(json.dumps(hashes, indent=2) + "\n")
    with (output / "image.log").open("w") as log:
        subprocess.run(["python3", "tools/make_image.py", "--esp-tree", str(stage),
                        "--size-mib", "2048", "--root-fs", "ext2", "--out", str(output / "probe.vmdk"),
                        "--raw", str(output / "probe.raw"), "--esp-image", str(output / "esp.fat"),
                        "--root-image", str(output / "root.ext2")], cwd=ROOT, stdout=log,
                       stderr=subprocess.STDOUT, check=True,
                       env={**os.environ, "TMPDIR": str(temporary)})
    serial, qmp = output / "serial.log", output / "qmp.sock"
    command = ["qemu-system-x86_64", "-enable-kvm", "-cpu", "host", "-machine", "q35",
               "-m", "4096", "-smp", str(args.smp), "-bios", "/usr/share/edk2/x64/OVMF.4m.fd",
               "-display", "none", "-serial", f"file:{serial}", "-device", "VGA,xres=1280,yres=720",
               "-drive", f"file={output / 'probe.raw'},format=raw,if=ide,snapshot=on",
               "-qmp", f"unix:{qmp},server=on,wait=off", "-no-reboot", "-no-shutdown"]
    if args.trace_storage:
        events = output / "trace-events"
        events.write_text("ide_bus_exec_cmd\npci_nvme_flush_ns\n")
        command += ["-trace", f"events={events},file={output / 'storage-trace.log'}"]
    (output / "run.json").write_text(json.dumps({"command": command, "probe": str(args.probe.resolve()),
        "marker": args.marker, "pam_unix_fixture": args.unix, "official_sudo": args.sudo,
        "host_kernel": os.uname().release}, indent=2) + "\n")
    with (output / "qemu.log").open("w") as log:
        process = subprocess.Popen(command, stdout=log, stderr=log)
        text = ""
        try:
            deadline = time.monotonic() + args.timeout
            while process.poll() is None and time.monotonic() < deadline:
                text = serial.read_text(errors="replace") if serial.exists() else ""
                if f"[{args.marker}] DONE" in text:
                    break
                time.sleep(.2)
        finally:
            if process.poll() is None:
                qmp_quit(qmp, process)
    print("\n".join(line for line in text.splitlines() if f"[{args.marker}]" in line))
    topology = re.search(r"SMP topology CPUs=(\d+).*discovered=(\d+)", text)
    observed = int(topology[1]) if topology else None
    ready = re.search(r"SMP ready online=(\d+)/(\d+)", text)
    online = int(ready[1]) if ready else 1 if observed == 1 else None
    (output / "cpu-evidence.json").write_text(json.dumps({
        "requested_cpus": args.smp, "observed_topology_cpus": observed,
        "observed_online_cpus": online,
        "ap_scheduler_disabled": "AP scheduler disabled" in text,
        "smp_required": args.require_smp,
    }, indent=2) + "\n")
    print(f"CPU evidence: QEMU requested={args.smp}, kernel online={online}")
    if args.require_smp and (args.smp < 2 or observed != args.smp or online != args.smp or
                            "AP scheduler disabled" in text):
        raise SystemExit(f"requested SMP execution not established: {serial}")
    if f"[{args.marker}] DONE failures=0" not in text:
        raise SystemExit(f"{args.marker} guest failed or timed out: {serial}")
    if args.unix and "[pam-runtime] pam_unix exercised=1" not in text:
        raise SystemExit(f"pam_unix fixture did not execute: {serial}")
    print(f"PASS {args.marker} guest (not full sudoers/PAM acceptance): {serial}")


if __name__ == "__main__":
    main()
