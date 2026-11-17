#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise the production GRUB trial selector against an owned writable FAT disk.

This tests actual BIOS/UEFI GRUB reads and save_env writes, without substituting
a Python selector. It does not boot a kernel or prove whole-system rollback.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def environment(fields):
    data = ("# GRUB Environment Block\n" + "".join(f"{key}={value}\n" for key, value in fields)).encode()
    if len(data) >= 1024:
        raise ValueError("oversized environment")
    return data + b"#" * (1024 - len(data))


def digest(data):
    return hashlib.sha256(data).hexdigest()


def write_slot(tree, name, generation, suffix, confirmed, remaining):
    release = f"5.3.3-ko1-{suffix}"
    abi = suffix + "0" * 52
    kernel, initramfs = (b"complete-kernel-" + suffix.encode(), b"complete-initramfs-" + suffix.encode())
    fields = [("reliefos_generation", generation), ("reliefos_release", release),
              ("reliefos_module_abi", abi), ("reliefos_kernel_sha256", digest(kernel)),
              ("reliefos_initramfs_sha256", digest(initramfs))]
    state = tree / "reliefos/state"
    state.mkdir(parents=True, exist_ok=True)
    record = environment(fields)
    (state / f"kernel-{name}.env").write_bytes(record)
    (state / f"kernel-{name}.sha256").write_text(f"{digest(record)}  kernel-{name}.env\n")
    (state / f"kernel-{name}.health").write_bytes(environment([
        ("reliefos_boot_generation", generation), ("reliefos_boot_confirmed", confirmed),
        ("reliefos_boot_remaining", remaining)]))
    version = tree / "reliefos/versions" / release / digest(initramfs)
    version.mkdir(parents=True)
    template = environment([("reliefos_generation", 1), *fields[1:]])
    (version / "boot.env").write_bytes(template)
    (version / "kernel.sys").write_bytes(kernel)
    (version / "initramfs.cpio").write_bytes(initramfs)
    (version / "SHA256SUMS").write_text(
        f"{digest(kernel)}  kernel.sys\n{digest(initramfs)}  initramfs.cpio\n{digest(template)}  boot.env\n")


def copy_to_disk(disk, source, destination):
    subprocess.run(["mcopy", "-o", "-i", disk, source, "::" + destination], check=True, capture_output=True)


def read_health(disk, work, name):
    target = work / f"observed-{name}.health"
    subprocess.run(["mcopy", "-o", "-i", disk, f"::/reliefos/state/kernel-{name}.health", target],
                   check=True, capture_output=True)
    return target.read_bytes()


def boot(args, firmware, iso, disk, name, expected, *, fail_writes=False):
    output = args.output / firmware / name
    output.mkdir(parents=True)
    serial = output / "serial.log"
    disk_source = str(disk)
    if fail_writes:
        # Fail the health record's data sector specifically. Firmware may write
        # unrelated FAT housekeeping before GRUB; those are outside this case.
        clusters = subprocess.check_output(["mshowfat", "-i", disk, "::/reliefos/state/kernel-B.health"], text=True)
        cluster = int(re.search(r"<(\d+)", clusters)[1])
        with disk.open("rb") as stream:
            bpb = stream.read(512)
        sector_size = struct.unpack_from("<H", bpb, 11)[0]
        reserved, root_entries, fat_sectors = (struct.unpack_from("<H", bpb, offset)[0] for offset in (14, 17, 22))
        first_data = reserved + bpb[16] * fat_sectors + (root_entries * 32 + sector_size - 1) // sector_size
        sector = (first_data + (cluster - 2) * bpb[13]) * sector_size // 512
        fault = output / "write-errors.conf"
        fault.write_text(f'[inject-error]\nevent = "write_aio"\niotype = "write"\nsector = "{sector}"\nerrno = "5"\nimmediately = "on"\nonce = "off"\n')
        disk_source = f"blkdebug:{fault}:{disk}"
    command = [args.qemu, "-machine", "q35", "-accel", "tcg", "-m", "256", "-smp", "1",
               "-display", "none", "-serial", f"file:{serial}", "-monitor", "none", "-no-reboot",
               "-drive", f"file={disk_source},format=raw,if=ide",
               "-cdrom", str(iso), "-boot", "d"]
    if firmware == "uefi":
        variables = output / "OVMF_VARS.fd"
        shutil.copy2(args.ovmf_vars, variables)
        command += ["-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={args.ovmf}",
                    "-drive", f"if=pflash,format=raw,unit=1,file={variables}"]
    (output / "command.json").write_text(json.dumps(command, indent=2) + "\n")
    with (output / "qemu.log").open("w") as log:
        process = subprocess.Popen(command, stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + args.timeout
            while time.monotonic() < deadline:
                observed = serial.read_text(errors="replace") if serial.exists() else ""
                if "BOOT_HEALTH_RESULT:" in observed:
                    assert expected in observed, observed
                    return observed
                if process.poll() is not None:
                    raise RuntimeError(f"QEMU exited {process.returncode}; see {output}")
                time.sleep(.1)
            raise TimeoutError(f"No GRUB health result; see {serial}")
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=10)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--grub-host", type=Path, default=ROOT / "out/ci-native/grub-host")
    parser.add_argument("--grub-efi", type=Path, default=ROOT / "out/ci-native/grub-efi/lib/grub/x86_64-efi")
    parser.add_argument("--ovmf", type=Path, default=next((path for path in (
        Path("/usr/share/OVMF/OVMF_CODE.fd"), Path("/usr/share/OVMF/OVMF_CODE_4M.fd")) if path.is_file()),
        Path("/usr/share/OVMF/OVMF_CODE_4M.fd")))
    parser.add_argument("--ovmf-vars", type=Path, help="matching OVMF variable template; copied privately per boot")
    parser.add_argument("--qemu", default=os.environ.get("QEMU", "qemu-system-x86_64"))
    parser.add_argument("--timeout", type=int, default=45)
    args = parser.parse_args()
    if args.ovmf_vars is None:
        args.ovmf_vars = args.ovmf.with_name("OVMF_VARS_4M.fd" if "4M" in args.ovmf.name else "OVMF_VARS.fd")
    for path in (args.ovmf, args.ovmf_vars):
        if not path.is_file():
            parser.error(f"missing OVMF firmware input: {path}")
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    stage = args.output / "tree"
    (stage / "grub").mkdir(parents=True)
    shutil.copy2(ROOT / "boot/grub/kernel-slots.cfg", stage / "grub/kernel-slots.cfg")
    (stage / "boot-health-test.marker").write_text("ReliefOS owned GRUB health acceptance\n")
    write_slot(stage, "A", 1, "0123456789ab", 1, 0)
    write_slot(stage, "B", 2, "fedcba987654", 0, 1)
    (stage / "reliefos/state/boot-health-v1").write_text("ReliefOS boot health v1\n")
    image = args.output / "baseline.fat"
    with image.open("xb") as stream:
        stream.truncate(32 * 1024**2)
    subprocess.run(["mkfs.vfat", "-F", "16", image], check=True, capture_output=True)
    for member in stage.iterdir():
        subprocess.run(["mcopy", "-s", "-i", image, member, "::/"], check=True, capture_output=True)
    iso_tree = args.output / "iso"
    (iso_tree / "boot/grub").mkdir(parents=True)
    (iso_tree / "boot/grub/grub.cfg").write_text("""serial --unit=0 --speed=115200
terminal_input serial
terminal_output serial
insmod fat
insmod search_fs_file
search --no-floppy --file --set=root /boot-health-test.marker
set reliefos_directory=
source ($root)/grub/kernel-slots.cfg
if reliefos_prepare_boot; then
    echo BOOT_HEALTH_RESULT:slot=$reliefos_selected,generation=$reliefos_selected_generation
else
    echo BOOT_HEALTH_RESULT:refused
fi
halt
""")
    results = []
    for firmware, modules in (("bios", args.grub_host / "lib/grub/i386-pc"), ("uefi", args.grub_efi)):
        if not modules.is_dir():
            raise FileNotFoundError(modules)
        iso = args.output / f"{firmware}.iso"
        with (args.output / f"{firmware}-mkrescue.log").open("w") as log:
            if firmware == "bios":
                command = [str(args.grub_host / "bin/grub-mkrescue"), "-d", str(modules), "--fonts=", "--locales=", "--themes=",
                           "--modules=serial terminal fat search_fs_file loadenv hashsum gcry_sha256 regexp test halt", "-o", str(iso), str(iso_tree)]
                subprocess.run(command, check=True, stdout=log, stderr=log)
            else:
                # Match boot-stage.sh's standalone EFI construction. mkrescue
                # otherwise requires a host-installed Unicode font that the
                # project's deliberately minimal GRUB host install omits.
                executable = args.output / "BOOTX64.EFI"
                subprocess.run([str(args.grub_host / "bin/grub-mkstandalone"), "-d", str(modules), "-O", "x86_64-efi",
                    "--fonts=", "--locales=", "--themes=",
                    "--modules=serial terminal fat search_fs_file loadenv hashsum gcry_sha256 regexp test halt",
                    "-o", str(executable), f"boot/grub/grub.cfg={iso_tree / 'boot/grub/grub.cfg'}"],
                    check=True, stdout=log, stderr=log)
                efi_disk = iso_tree / "efi.img"
                with efi_disk.open("xb") as stream:
                    stream.truncate(16 * 1024**2)
                subprocess.run(["mkfs.vfat", "-F", "16", efi_disk], check=True, stdout=log, stderr=log)
                subprocess.run(["mmd", "-i", efi_disk, "::/EFI", "::/EFI/BOOT"], check=True, stdout=log, stderr=log)
                subprocess.run(["mcopy", "-i", efi_disk, executable, "::/EFI/BOOT/BOOTX64.EFI"], check=True, stdout=log, stderr=log)
                subprocess.run(["xorriso", "-as", "mkisofs", "-R", "-J", "-e", "efi.img", "-no-emul-boot",
                                "-o", iso, iso_tree], check=True, stdout=log, stderr=log)
        disk = args.output / f"{firmware}.fat"
        shutil.copyfile(image, disk)
        boot(args, firmware, iso, disk, "first-trial", "BOOT_HEALTH_RESULT:slot=B,generation=2")
        consumed = read_health(disk, args.output, "B")
        assert b"reliefos_boot_generation=2\n" in consumed and b"reliefos_boot_confirmed=0\n" in consumed
        assert b"reliefos_boot_remaining=0\n" in consumed
        assert consumed == environment([("reliefos_boot_generation", 2), ("reliefos_boot_confirmed", 0),
                                        ("reliefos_boot_remaining", 0)])
        boot(args, firmware, iso, disk, "unconfirmed-fallback", "BOOT_HEALTH_RESULT:slot=A,generation=1")
        confirmed = args.output / f"{firmware}-confirmed.health"
        confirmed.write_bytes(consumed.replace(b"reliefos_boot_confirmed=0", b"reliefos_boot_confirmed=1"))
        copy_to_disk(disk, confirmed, "/reliefos/state/kernel-B.health")
        boot(args, firmware, iso, disk, "confirmed-new", "BOOT_HEALTH_RESULT:slot=B,generation=2")
        corrupt = args.output / f"{firmware}-corrupt.health"
        corrupt.write_bytes(consumed.replace(b"reliefos_boot_generation=2", b"reliefos_boot_generation=3"))
        copy_to_disk(disk, corrupt, "/reliefos/state/kernel-B.health")
        boot(args, firmware, iso, disk, "stale-health-fallback", "BOOT_HEALTH_RESULT:slot=A,generation=1")
        shutil.copyfile(image, disk)
        boot(args, firmware, iso, disk, "failed-write-trial-fallback", "BOOT_HEALTH_RESULT:slot=A,generation=1", fail_writes=True)
        results.append({"firmware": firmware, "trial_consumed": True, "unconfirmed_fallback": True,
                        "confirmation_selects_new": True, "stale_state_fallback": True, "write_failure_refuses_trial": True})
    (args.output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    print("PASS actual GRUB BIOS/UEFI: trial consumption, failed-boot fallback, confirmation, stale state and write-failure refusal")


if __name__ == "__main__":
    main()
