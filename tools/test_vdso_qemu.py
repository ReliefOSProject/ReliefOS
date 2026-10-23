#!/usr/bin/env python3
"""Run pinned-musl vDSO probes on a private copy of a ReliefOS disk image."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

from run_gcc_probe_qemu import qmp_quit

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_IMAGE = ROOT / "out/xorg-vmware-debug/images/reliefos.vmdk"
OVMF = Path("/usr/share/edk2/x64/OVMF.4m.fd")
PROBE_SOURCE = ROOT / "kernel/reliefnt/tools/tests/vdso_guest_test.c"
SERVICE_PATH = "/etc/init.d/reliefos-vdso-guest"
SERVICE_LINK = "/etc/runlevels/default/reliefos-vdso-guest"
GUEST_TEST_DIR = "/usr/lib/reliefos/tests"
PAGE_SIZE = 512


SERVICE = """#!/sbin/openrc-run
description="ReliefOS vDSO guest probe"
# The probes use only injected binaries/libraries and the serial device. They
# do not require reliefos-runtime's application state directories.
depend() { after localmount; before reliefos-session; }
start() {
    /usr/lib/reliefos/tests/vdso-static > /dev/ttyS0 2>&1
    static_status=$?
    /usr/lib/reliefos/tests/vdso-dynamic > /dev/ttyS0 2>&1
    dynamic_status=$?
    echo "[vdso-runner] DONE static=$static_status dynamic=$dynamic_status" > /dev/ttyS0
    return 0
}
"""


def run(command: list[object], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run([str(value) for value in command], cwd=ROOT,
                          check=True, **kwargs)


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def default_sysroot() -> Path:
    explicit = ROOT / "build/musl/sysroot"
    if (explicit / "include/time.h").is_file() and (explicit / "lib/libc.so").is_file():
        return explicit
    candidates = [
        path for path in (ROOT / "out").glob("*/sysroot/musl")
        if (path / "include/time.h").is_file() and (path / "lib/libc.so").is_file()
    ]
    if not candidates:
        raise SystemExit("no pinned-musl sysroot found; pass --sysroot")
    return max(candidates, key=lambda path: path.stat().st_mtime)


def compile_probes(output: Path, sysroot: Path) -> tuple[Path, Path]:
    static_probe = output / "vdso-static"
    dynamic_probe = output / "vdso-dynamic"
    resource = subprocess.check_output(["clang", "-print-resource-dir"], text=True).strip()
    object_file = output / "vdso_guest_test.o"
    run([
        "clang", "--target=x86_64-linux-musl",
        "-nostdinc", "-isystem", sysroot / "include",
        "-isystem", Path(resource) / "include",
        "-D_GNU_SOURCE", "-DRELIEFOS_USE_MUSL", "-mno-avx", "-mno-avx2",
        "-O2", "-Wall", "-Wextra", "-Werror", "-fno-stack-protector",
        "-c", PROBE_SOURCE, "-o", object_file,
    ])
    run([
        "ld.lld", "-static", "--build-id=none", "-z", "max-page-size=0x1000",
        "--image-base=0x4000000", "-o", static_probe,
        sysroot / "lib/crt1.o", sysroot / "lib/crti.o", object_file,
        sysroot / "lib/mimalloc.o", "--start-group", sysroot / "lib/libc.a",
        "--end-group", sysroot / "lib/crtn.o",
    ])
    run([
        "ld.lld", "-pie", "--build-id=none", "-z", "max-page-size=0x1000",
        "--dynamic-linker", "/lib/ld-musl-x86_64.so.1",
        "-rpath", "/lib:/usr/lib", "-o", dynamic_probe,
        sysroot / "lib/Scrt1.o", sysroot / "lib/crti.o", object_file,
        "-L", sysroot / "lib", "-l:libmimalloc.so.3", "-lc",
        sysroot / "lib/crtn.o",
    ])
    static_program_headers = subprocess.check_output(
        ["readelf", "-l", static_probe], text=True)
    dynamic_program_headers = subprocess.check_output(
        ["readelf", "-l", dynamic_probe], text=True)
    dynamic_entries = subprocess.check_output(
        ["readelf", "-d", dynamic_probe], text=True)
    if "INTERP" in static_program_headers:
        raise RuntimeError("static pinned-musl probe unexpectedly has PT_INTERP")
    if "/lib/ld-musl-x86_64.so.1" not in dynamic_program_headers:
        raise RuntimeError("dynamic pinned-musl probe has the wrong PT_INTERP")
    if "[libmimalloc.so.3]" not in dynamic_entries or "[libc.so]" not in dynamic_entries:
        raise RuntimeError("dynamic probe is missing pinned musl or mimalloc DT_NEEDED")
    if str(sysroot) in dynamic_entries:
        raise RuntimeError("dynamic probe leaked the host sysroot into DT_NEEDED")
    return static_probe, dynamic_probe


def gpt_partitions(raw: Path) -> list[dict[str, int | str]]:
    result: list[dict[str, int | str]] = []
    with raw.open("rb") as disk:
        disk.seek(PAGE_SIZE)
        header = disk.read(92)
        if header[:8] != b"EFI PART":
            raise ValueError("input disk has no primary GPT header")
        entries_lba = struct.unpack_from("<Q", header, 72)[0]
        entry_count, entry_size = struct.unpack_from("<II", header, 80)
        if entry_count > 4096 or entry_size < 128 or entry_size > 4096:
            raise ValueError("input disk has invalid GPT entry dimensions")
        for index in range(entry_count):
            disk.seek(entries_lba * PAGE_SIZE + index * entry_size)
            entry = disk.read(entry_size)
            if len(entry) != entry_size or not any(entry):
                continue
            first, last = struct.unpack_from("<QQ", entry, 32)
            name = entry[56:128].decode("utf-16le", errors="replace").rstrip("\0")
            result.append({"index": index, "first": first, "last": last, "name": name})
    return result


def private_raw_image(image: Path, output: Path) -> tuple[Path, dict[str, int | str]]:
    raw = output / "guest.raw"
    with (output / "convert.log").open("w") as log:
        run(["qemu-img", "convert", "-f", "vmdk", "-O", "raw", image, raw],
            stdout=log, stderr=log)
    partitions = gpt_partitions(raw)
    esp = next((partition for partition in partitions
                if partition["name"] == "RELIEFOS_ESP"), None)
    root = next((partition for partition in partitions
                 if partition["name"] == "RELIEFOS_ROOT"), None)
    if esp is None or root is None:
        raise ValueError("input image needs RELIEFOS_ESP and RELIEFOS_ROOT GPT partitions")
    return raw, {"esp_first": int(esp["first"]), "root_first": int(root["first"]),
                 "root_last": int(root["last"])}


def extract_root(raw: Path, layout: dict[str, int | str], output: Path) -> Path:
    root_image = output / "guest-root.ext4"
    first, last = int(layout["root_first"]), int(layout["root_last"])
    remaining = (last - first + 1) * PAGE_SIZE
    with raw.open("rb") as disk, root_image.open("wb") as root:
        disk.seek(first * PAGE_SIZE)
        while remaining:
            block = disk.read(min(8 * 1024 * 1024, remaining))
            if not block:
                raise ValueError("input disk ended inside its root partition")
            root.write(block)
            remaining -= len(block)
    return root_image


def debugfs_exists(root_image: Path, guest_path: str) -> bool:
    status = subprocess.run(
        ["debugfs", "-R", f"stat {guest_path}", str(root_image)],
        cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        check=False,
    )
    if "File not found" in status.stdout:
        return False
    if status.returncode != 0:
        raise RuntimeError(f"cannot inspect {guest_path} in private root: {status.stdout}")
    return "Inode:" in status.stdout


def add_missing_root_directory(root_image: Path, guest_path: str,
                               commands: list[str]) -> None:
    if debugfs_exists(root_image, guest_path):
        return
    parent = str(Path(guest_path).parent)
    if parent != guest_path:
        add_missing_root_directory(root_image, parent, commands)
    commands.append(f"mkdir {guest_path}")


def inject_root(root_image: Path, output: Path, sysroot: Path,
                static_probe: Path, dynamic_probe: Path,
                require_hres: bool = False) -> None:
    commands_path = output / "debugfs.commands"
    commands: list[str] = []
    add_missing_root_directory(root_image, GUEST_TEST_DIR, commands)
    for guest_path in (
        "/lib/ld-musl-x86_64.so.1", "/lib/libc.so", "/lib/libmimalloc.so.3",
        GUEST_TEST_DIR + "/vdso-static", GUEST_TEST_DIR + "/vdso-dynamic",
        SERVICE_PATH, SERVICE_LINK,
    ):
        if debugfs_exists(root_image, guest_path):
            commands.append(f"rm {guest_path}")
    commands.extend([
        f'write "{sysroot / "lib/libc.so"}" /lib/ld-musl-x86_64.so.1',
        'set_inode_field /lib/ld-musl-x86_64.so.1 mode 0100755',
        f'write "{sysroot / "lib/libc.so"}" /lib/libc.so',
        'set_inode_field /lib/libc.so mode 0100755',
        f'write "{sysroot / "lib/libmimalloc.so.3"}" /lib/libmimalloc.so.3',
        'set_inode_field /lib/libmimalloc.so.3 mode 0100755',
        f'write "{static_probe}" {GUEST_TEST_DIR}/vdso-static',
        f'set_inode_field {GUEST_TEST_DIR}/vdso-static mode 0100755',
        f'write "{dynamic_probe}" {GUEST_TEST_DIR}/vdso-dynamic',
        f'set_inode_field {GUEST_TEST_DIR}/vdso-dynamic mode 0100755',
    ])
    service_file = output / "reliefos-vdso-guest"
    service = SERVICE
    if require_hres:
        service = service.replace("/vdso-static >", "/vdso-static --require-hres >")
        service = service.replace("/vdso-dynamic >", "/vdso-dynamic --require-hres >")
    service_file.write_text(service, encoding="ascii")
    commands.extend([
        f'write "{service_file}" {SERVICE_PATH}',
        f'set_inode_field {SERVICE_PATH} mode 0100755',
        f'symlink {SERVICE_LINK} /etc/init.d/reliefos-vdso-guest',
    ])
    with commands_path.open("w", encoding="utf-8") as stream:
        stream.write("\n".join(commands) + "\n")
    with (output / "debugfs.log").open("w") as log:
        run(["debugfs", "-w", "-f", commands_path, root_image],
            stdout=log, stderr=subprocess.STDOUT)


def install_private_kernel(raw: Path, esp_first: int, kernel: Path,
                           output: Path) -> dict[str, object]:
    offset = esp_first * PAGE_SIZE
    image_arg = f"{raw}@@{offset}"
    source_hash = file_sha256(kernel)
    readbacks: dict[str, str] = {}
    for guest_path in ("::/reliefos/kernel.sys", "::/leonos/kernel.sys"):
        run(["mcopy", "-o", "-i", image_arg, kernel, guest_path])
        safe_name = guest_path.replace("::/", "").replace("/", "-")
        readback = output / f"readback-{safe_name}"
        run(["mcopy", "-i", image_arg, guest_path, readback])
        read_hash = file_sha256(readback)
        if read_hash != source_hash:
            raise RuntimeError(f"kernel read-back hash mismatch for {guest_path}")
        readbacks[guest_path] = read_hash
    return {"source_sha256": source_hash, "readback_sha256": readbacks}


def put_root_back(raw: Path, root_image: Path,
                  layout: dict[str, int | str]) -> None:
    with raw.open("r+b") as disk, root_image.open("rb") as root:
        disk.seek(int(layout["root_first"]) * PAGE_SIZE)
        shutil.copyfileobj(root, disk, 8 * 1024 * 1024)


def choose_accelerator(requested: str) -> str:
    if requested != "auto":
        if requested == "kvm" and not Path("/dev/kvm").exists():
            raise SystemExit("KVM requested but /dev/kvm is unavailable")
        return requested
    return "kvm" if Path("/dev/kvm").exists() else "tcg"


def qemu_command(raw: Path, serial: Path, qmp: Path, smp: int,
                 accelerator: str, cpu: str | None, invtsc: bool) -> list[str]:
    selected_cpu = cpu or ("host" if accelerator == "kvm" else "max")
    if invtsc and "invtsc" not in selected_cpu:
        selected_cpu += ",+invtsc"
    return [
        "qemu-system-x86_64", "-machine", "q35", "-accel", accelerator,
        "-cpu", selected_cpu, "-m", "4096", "-smp", str(smp),
        "-bios", str(OVMF), "-display", "none", "-serial", f"file:{serial}",
        "-nic", "none", "-drive", f"file={raw},format=raw,if=ide",
        "-qmp", f"unix:{qmp},server=on,wait=off",
        "-no-reboot", "-no-shutdown",
    ]


def boot(raw: Path, output: Path, command: list[str], timeout: float) -> str:
    serial = output / "serial.log"
    qmp = output / "qmp.sock"
    errors = (output / "qemu.log").open("w")
    process = subprocess.Popen(command, cwd=ROOT, stdout=errors, stderr=errors,
                               env={**os.environ, "TMPDIR": str(ROOT / "out/tmp")})
    text = ""
    deadline = time.monotonic() + timeout
    try:
        while time.monotonic() < deadline:
            text = serial.read_text(errors="replace") if serial.exists() else ""
            if "[vdso-runner] DONE" in text or "KERNEL PANIC" in text:
                break
            if process.poll() is not None:
                break
            time.sleep(0.2)
    finally:
        if process.poll() is None:
            try:
                qmp_quit(qmp, process)
            except Exception:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
        errors.close()
    return serial.read_text(errors="replace") if serial.exists() else text


def expected_missing_vdso(text: str) -> bool:
    missing = text.count("[vdso] FAIL missing AT_SYSINFO_EHDR")
    completed = re.findall(r"\[vdso\] DONE failures=(\d+)", text)
    runner = re.search(r"\[vdso-runner\] DONE static=(\d+) dynamic=(\d+)", text)
    return (missing == 2 and len(completed) == 2 and completed == ["1", "1"] and
            runner is not None and runner.groups() == ("1", "1") and
            "KERNEL PANIC" not in text)


def expected_success(text: str) -> bool:
    done = re.findall(r"\[vdso\] DONE failures=(\d+)", text)
    runner = re.search(r"\[vdso-runner\] DONE static=(\d+) dynamic=(\d+)", text)
    return (len(done) == 2 and done == ["0", "0"] and runner is not None and
            runner.groups() == ("0", "0") and
            "[vdso] FAIL" not in text and "KERNEL PANIC" not in text)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, default=DEFAULT_IMAGE,
                        help="input VMDK; it is converted before any guest files are injected")
    parser.add_argument("--kernel", type=Path,
                        help="optional standalone kernel.sys copied to both ESP boot paths in the private raw image")
    parser.add_argument("--sysroot", type=Path,
                        help="pinned musl sysroot used for both guest binaries")
    parser.add_argument("--output", type=Path,
                        help="new evidence directory below build/; defaults to a unique build/vdso-guest-* directory")
    parser.add_argument("--smp", type=int, choices=(1, 2), default=1,
                        help="QEMU vCPU count")
    parser.add_argument("--accel", choices=("auto", "kvm", "tcg"), default="auto")
    parser.add_argument("--cpu", help="QEMU CPU model; defaults to host for KVM or max for TCG")
    parser.add_argument("--invtsc", action="store_true",
                        help="append the optional invtsc CPU flag")
    parser.add_argument("--require-hres", action="store_true",
                        help="wait for calibration and require both probes to exercise TSC fast paths")
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("--expect-missing-vdso", action="store_true",
                        help="accept only the intentional pre-implementation RED: both probes report missing AT_SYSINFO_EHDR")
    args = parser.parse_args()

    image = args.image.resolve()
    sysroot = (args.sysroot or default_sysroot()).resolve()
    kernel = args.kernel.resolve() if args.kernel else None
    for path, label in ((image, "image"), (sysroot / "include/time.h", "musl headers"),
                        (sysroot / "lib/libc.so", "pinned musl runtime"),
                        (sysroot / "lib/libmimalloc.so.3", "pinned mimalloc runtime"),
                        (sysroot / "lib/mimalloc.o", "pinned static mimalloc object")):
        if not path.exists():
            parser.error(f"missing {label}: {path}")
    if kernel and not kernel.is_file():
        parser.error(f"missing standalone kernel: {kernel}")
    if args.output:
        output = args.output.resolve()
        build_root = (ROOT / "build").resolve()
        if not output.is_relative_to(build_root) or output.exists():
            parser.error("--output must be a new directory below build/")
        output.mkdir(parents=True)
    else:
        output = Path(tempfile.mkdtemp(prefix="vdso-guest-", dir=ROOT / "build"))
    temporary_root = ROOT / "out/tmp"
    temporary_root.mkdir(parents=True, exist_ok=True)
    os.environ["TMPDIR"] = str(temporary_root)
    source_hash_before = file_sha256(image)
    static_probe, dynamic_probe = compile_probes(output, sysroot)
    raw, layout = private_raw_image(image, output)
    root_image = extract_root(raw, layout, output)
    inject_root(root_image, output, sysroot, static_probe, dynamic_probe,
                require_hres=args.require_hres)
    put_root_back(raw, root_image, layout)
    kernel_hashes = None
    if kernel:
        kernel_hashes = install_private_kernel(
            raw, int(layout["esp_first"]), kernel, output)

    accelerator = choose_accelerator(args.accel)
    command = qemu_command(raw, output / "serial.log", output / "qmp.sock",
                           args.smp, accelerator, args.cpu, args.invtsc)
    run_metadata = {
        "input_image": str(image),
        "input_image_sha256_before": source_hash_before,
        "input_image_sha256_after": None,
        "kernel": str(kernel) if kernel else None,
        "kernel_hashes": kernel_hashes,
        "sysroot": str(sysroot),
        "sysroot_libc_sha256": file_sha256(sysroot / "lib/libc.so"),
        "static_probe": str(static_probe),
        "dynamic_probe": str(dynamic_probe),
        "requested_smp": args.smp,
        "accelerator": accelerator,
        "cpu": command[command.index("-cpu") + 1],
        "invtsc": args.invtsc,
        "require_hres": args.require_hres,
        "command": command,
    }
    text = boot(raw, output, command, args.timeout)
    image_hash_after = file_sha256(image)
    run_metadata["input_image_sha256_after"] = image_hash_after
    run_metadata["input_image_unchanged"] = source_hash_before == image_hash_after
    (output / "run.json").write_text(json.dumps(run_metadata, indent=2) + "\n")

    for line in text.splitlines():
        if "[vdso]" in line or "[vdso-runner]" in line:
            print(line)
    if source_hash_before != image_hash_after:
        raise SystemExit(f"input image changed while testing: {image}")
    if args.expect_missing_vdso:
        if expected_missing_vdso(text):
            print(f"EXPECTED RED: both pinned-musl probes fail on missing AT_SYSINFO_EHDR ({output / 'serial.log'})")
            return 0
        raise SystemExit(f"expected both probes to fail only on missing AT_SYSINFO_EHDR: {output / 'serial.log'}")
    if not expected_success(text):
        raise SystemExit(f"vDSO guest probes failed or timed out: {output / 'serial.log'}")
    print(f"PASS vDSO guest probes: {output / 'serial.log'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
