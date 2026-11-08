#!/usr/bin/env python3
"""Install two systems onto one shared ESP and boot their actual native payloads."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time
import uuid

from test_dual_boot_qemu import boot, boot_evidence, wait_for, wait_xdm
from test_installer_accounts_qemu import install_tty
from test_vt_qemu import ocr, wait_text

ROOT = Path(__file__).resolve().parents[1]
FOREIGN = b"foreign EFI fallback -- must survive both installations\n"
WINDOWS = b"foreign Microsoft EFI payload -- must survive\n"


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def esp_bytes(disk, partition, name):
    return subprocess.check_output(["mtype", "-i", f"{disk}@@{partition['start'] * 512}", "::/" + name])


def compose_disk(output):
    disk = output / "two-installations.raw"
    with disk.open("xb") as stream:
        stream.truncate(7 * 1024**3)
    ids = {name: str(uuid.uuid4()) for name in ("esp", "first", "second")}
    run("sfdisk", str(disk), input=(
        "label: gpt\nunit: sectors\n\n"
        "start=2048,size=2048,type=21686148-6449-6E6F-744E-656564454649,name=BIOS\n"
        f"start=4096,size=1048576,type=U,uuid={ids['esp']},name=SharedESP\n"
        f"size=5242880,type=L,uuid={ids['first']},name=FirstReliefOS\n"
        f"size=5242880,type=L,uuid={ids['second']},name=SecondReliefOS\n"
    ), text=True, stdout=subprocess.DEVNULL)
    table = json.loads(subprocess.check_output(["sfdisk", "--json", disk]))["partitiontable"]
    esp = table["partitions"][1]
    fat = output / "shared-esp.fat"
    with fat.open("xb") as stream:
        stream.truncate(esp["size"] * 512)
    run("mkfs.fat", "--invariant", "-F", "32", str(fat), stdout=subprocess.DEVNULL)
    tree = output / "foreign"
    (tree / "EFI/BOOT").mkdir(parents=True)
    (tree / "EFI/Microsoft/Boot").mkdir(parents=True)
    (tree / "EFI/BOOT/BOOTX64.EFI").write_bytes(FOREIGN)
    (tree / "EFI/Microsoft/Boot/bootmgfw.efi").write_bytes(WINDOWS)
    run("mcopy", "-s", "-i", str(fat), str(tree / "EFI"), "::/")
    run("dd", f"if={fat}", f"of={disk}", "bs=512", f"seek={esp['start']}", "conv=notrunc,sparse", "status=none")
    fat.unlink()
    return disk, esp, ids


def assert_foreign(disk, esp):
    assert esp_bytes(disk, esp, "EFI/BOOT/BOOTX64.EFI") == FOREIGN
    assert esp_bytes(disk, esp, "EFI/Microsoft/Boot/bootmgfw.efi") == WINDOWS


def efi_chooser(output, disk, esp, identity):
    """Use a separate boot manager to select a named EFI file without touching its disk."""
    tree = output / "chooser"
    (tree / "EFI/BOOT").mkdir(parents=True)
    loader = f"EFI/ReliefOS/{identity}/grubx64.efi"
    (tree / "EFI/BOOT/BOOTX64.EFI").write_bytes(esp_bytes(disk, esp, loader))
    digest = hashlib.sha256((tree / "EFI/BOOT/BOOTX64.EFI").read_bytes()).hexdigest()
    (tree / "EFI/BOOT/reliefos-owner").write_text(f"ReliefOS fallback v1\n{identity}\n{digest}\n")
    (tree / "EFI/BOOT/grub.cfg").write_text(
        f"insmod chain\nsearch --no-floppy --file /{loader} --set=root\n"
        f"chainloader ($root)/{loader}\nboot\n")
    image = output / "efi-selector.raw"
    with image.open("xb") as stream:
        stream.truncate(64 * 1024**2)
    run("mkfs.fat", "--invariant", "-F", "32", str(image), stdout=subprocess.DEVNULL)
    run("mcopy", "-s", "-i", str(image), str(tree / "EFI"), "::/")
    return image


def check_update_selection(probe, serial, process, identities, timeout, after_install=True):
    """Exercise the native update worker against both roots and an ambiguous choice."""
    if after_install:
        probe.text("n")
        probe.key("ret")
        time.sleep(1)
        probe.text("umount /target/boot; umount /target/efi; umount /target; echo __MULTI_UNMOUNT_$? > /dev/ttyS0")
        probe.key("ret")
        wait_for(serial, process, lambda text: "__MULTI_UNMOUNT_0" in text,
                 "release the fresh install mounts before read-only update checks", timeout)
    checked = {}
    for name in ("first", "second", "ambiguous"):
        choice = "" if name == "ambiguous" else f" {identities[name]} {identities['esp']}"
        marker = "__MULTI_UPDATE_" + name + "__"
        before = serial.stat().st_size
        probe.text(f"installer --update-worker check /dev/sda{choice} 2>&1|tee /dev/ttyS0; echo {marker} > /dev/ttyS0")
        probe.key("ret")
        wait_for(serial, process, lambda text: marker in text, "native update selection " + name, timeout)
        observed = serial.read_bytes()[before:].decode(errors="replace")
        (probe.output / f"update-{name}.log").write_text(observed)
        if name == "ambiguous":
            assert "error\t76\t" in observed, observed[-2000:]
        else:
            assert "error\t" not in observed, observed[-2000:]
            assert "info\troot_uuid\t" + identities[name] in observed
            assert "info\tesp_uuid\t" + identities["esp"] in observed
            checked[name] = dict(line.split("\t", 2)[1:] for line in observed.splitlines() if line.startswith("info\t"))
        print(f"PASS native updater selection: {name}", flush=True)
    apply_checked_update(probe, serial, process, checked["second"], timeout)


def apply_checked_update(probe, serial, process, info, timeout):
    """Apply immutable metadata; the native worker rechecks it before writes."""
    arguments = " ".join(info[field] for field in ("root_uuid", "esp_uuid", "index_sha256", "database_sha256", "world_sha256"))
    before = serial.stat().st_size
    probe.text(f"installer --update-worker apply /dev/sda {arguments} 3 2>&1|tee /dev/ttyS0; echo __MULTI_APPLY__ > /dev/ttyS0")
    probe.key("ret")
    wait_for(serial, process, lambda text: "__MULTI_APPLY__" in text, "native update of the second installation", timeout)
    observed = serial.read_bytes()[before:].decode(errors="replace")
    (probe.output / "update-apply-second.log").write_text(observed)
    assert "error\t" not in observed and "System update completed successfully" in observed, observed[-4000:]
    print("PASS native updater transaction on the second installation", flush=True)


def checked_fixture(directory, identities):
    """Resume a prepared disk only after all three native check results exist."""
    text = (directory / "update-two-roots/serial.log").read_text(errors="replace")
    first, rest = text.split("__MULTI_UPDATE_first__", 1)
    second, rest = rest.split("__MULTI_UPDATE_second__", 1)
    ambiguous = rest.split("__MULTI_UPDATE_ambiguous__", 1)[0]
    assert "error\t" not in first and "info\troot_uuid\t" + identities["first"] in first
    assert "error\t" not in second and "error\t76\t" in ambiguous
    info = dict(line.split("\t", 2)[1:] for line in second.splitlines() if line.startswith("info\t"))
    assert info["root_uuid"] == identities["second"] and info["esp_uuid"] == identities["esp"]
    for field in ("index_sha256", "database_sha256", "world_sha256"):
        assert len(info[field]) == 64 and all(c in "0123456789abcdef" for c in info[field])
    return info


def seed_second_fixture(source, disk, esp, identities, output):
    """Reuse a built root for update isolation without repeating the root copy."""
    table = json.loads(subprocess.check_output(["sfdisk", "--json", source]))["partitiontable"]
    primary = json.loads(subprocess.check_output(["sfdisk", "--json", disk]))["partitiontable"]
    root = next(p for p in table["partitions"] if p["type"].lower() == "0fc63daf-8483-4772-8e79-3d69d8477de4")
    source_esp = next(p for p in table["partitions"] if p["type"].lower() == "c12a7328-f81f-11d2-ba4b-00a0c93ec93b")
    destination = next(p for p in primary["partitions"] if p["uuid"].lower() == identities["second"])
    assert root["size"] <= destination["size"], "Root fixture must fit the second partition"
    image = output / "second-root.ext4"
    run("dd", f"if={source}", f"of={image}", "bs=1M", "iflag=skip_bytes,count_bytes",
        f"skip={root['start'] * 512}", f"count={root['size'] * 512}", "conv=sparse", "status=none")
    with image.open("r+b") as stream:
        stream.truncate(destination["size"] * 512)
    run("resize2fs", str(image), stdout=subprocess.DEVNULL)
    fstab = output / "second-fstab"
    fstab.write_text(f"/dev/disk/by-partuuid/{identities['second']} / ext4 defaults 0 1\n"
                     f"/dev/disk/by-partuuid/{identities['esp']} /efi vfat defaults 0 2\n"
                     f"/efi/reliefos/instances/{identities['second']} /boot none bind 0 0\n")
    run("debugfs", "-w", "-R", "mkdir /efi", str(image), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    run("debugfs", "-w", "-R", "rm /etc/fstab", str(image), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    run("debugfs", "-w", "-R", f"write {fstab} /etc/fstab", str(image), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    assert subprocess.check_output(["debugfs", "-R", "cat /etc/fstab", str(image)], stderr=subprocess.DEVNULL) == fstab.read_bytes()
    run("e2fsck", "-f", "-n", str(image), stdout=subprocess.DEVNULL)
    run("dd", f"if={image}", f"of={disk}", "bs=1M", "oflag=seek_bytes",
        f"seek={destination['start'] * 512}", "conv=notrunc", "status=none")
    image.unlink()
    tree = output / "second-boot-fixture"
    tree.mkdir()
    for name in ("grub", "reliefos", "leonos", "EFI"):
        run("mcopy", "-s", "-i", f"{source}@@{source_esp['start'] * 512}", "::/" + name, str(tree) + "/")
    (tree / "reliefos/config/root.cfg").write_text(f"set reliefos_root=PARTUUID={identities['second']}\n")
    (tree / "reliefos-instance.id").write_text(f"ReliefOS instance v1\n{identities['second']}\n")
    target = f"{disk}@@{esp['start'] * 512}"
    relative = "::/reliefos/instances/" + identities["second"]
    created = subprocess.run(["mmd", "-i", target, relative], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if created.returncode:
        assert esp_bytes(disk, esp, "reliefos/instances/" + identities["second"] + "/reliefos-instance.id") == (tree / "reliefos-instance.id").read_bytes()
    run("mcopy", "-o", "-s", "-i", target, *(str(p) for p in sorted(tree.iterdir())), relative + "/")


def enter_console(probe, serial, process):
    deadline = time.monotonic() + 45
    while time.monotonic() < deadline:
        probe.key("up")
        if "install reliefos" in ocr(probe.frame("console-entry")).lower().replace("0", "o"):
            break
    else:
        raise AssertionError("Installer menu did not appear")
    probe.key("down"); probe.key("ret")
    wait_for(serial, process, lambda text: "path=/bin/bash" in text, "installer console", 180)


def worker_disk(binary, output, library=None):
    """Carry the current installer independently of the package-version fixture."""
    disk, fat = output / "worker.raw", output / "worker.fat"
    with disk.open("xb") as stream:
        stream.truncate(66 * 1024**2)
    run("sfdisk", str(disk), input="label: gpt\nunit: sectors\n\nstart=2048,size=131072,type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7\n", text=True, stdout=subprocess.DEVNULL)
    with fat.open("xb") as stream:
        stream.truncate(64 * 1024**2)
    run("mkfs.fat", "--invariant", "-F", "32", str(fat), stdout=subprocess.DEVNULL)
    run("mcopy", "-i", str(fat), str(binary), "::/installer.elf")
    if library:
        run("mcopy", "-i", str(fat), str(library), "::/libreliefos.so.2")
    run("dd", f"if={fat}", f"of={disk}", "bs=512", "seek=2048", "conv=notrunc,sparse", "status=none")
    fat.unlink()
    return disk


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--images", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--install-timeout", type=int, default=7200)
    parser.add_argument("--first-installation", type=Path, help="Completed first-install test directory; only its copy is modified")
    parser.add_argument("--root-fixture", type=Path, help="Built disk root reused for the second installation/update case")
    parser.add_argument("--worker-binary", type=Path, help="Test the current worker against the fixed package-version fixture")
    parser.add_argument("--worker-library", type=Path, help="Current runtime DSO used with the worker binary")
    parser.add_argument("--checked-fixture", type=Path, help="Resume a prepared copied disk after recorded native target checks")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    if bool(args.first_installation) != bool(args.root_fixture):
        parser.error("--first-installation and --root-fixture are used together")
    if args.first_installation:
        previous = args.first_installation.resolve()
        identities = json.loads((previous / "identities.json").read_text())
        disk = output / "two-installations.raw"
        run("cp", "--reflink=auto", "--sparse=always", str(previous / disk.name), str(disk))
        table = json.loads(subprocess.check_output(["sfdisk", "--json", disk]))["partitiontable"]
        esp = next(p for p in table["partitions"] if p["uuid"].lower() == identities["esp"])
        assert_foreign(disk, esp)
        assert esp_bytes(disk, esp, f"reliefos/instances/{identities['first']}/reliefos/config/root.cfg") == f"set reliefos_root=PARTUUID={identities['first']}\n".encode()
    else:
        disk, esp, identities = compose_disk(output)
    (output / "identities.json").write_text(json.dumps(identities, indent=2) + "\n")
    first_files = None
    cases = () if args.first_installation else ((2, "first"), (3, "second"))
    for row, name in cases:
        with boot(output / f"install-{name}", "bios", disk=disk,
                  iso=args.images.resolve() / "reliefos-installer.iso") as session:
            install_tty(*session, skip_xorg=True, install_timeout=args.install_timeout,
                        partition_commands=("1", "esp", str(row), "root", "done"))
            if name == "second":
                check_update_selection(*session, identities, args.install_timeout)
        assert_foreign(disk, esp)
        identity = identities[name]
        base = f"reliefos/instances/{identity}"
        assert esp_bytes(disk, esp, base + "/reliefos/config/root.cfg") == f"set reliefos_root=PARTUUID={identity}\n".encode()
        files = {member: hashlib.sha256(esp_bytes(disk, esp, f"reliefos/instances/{identities['first']}/{member}")).hexdigest()
                 for member in ("reliefos/config/root.cfg", "reliefos/state/kernel-A.env", "grub/grub.cfg")}
        if first_files is None:
            first_files = files
        else:
            assert files == first_files, "Second install changed the first installation's boot state"
        print(f"PASS shared ESP installation: {name}", flush=True)
    if args.first_installation:
        members = ("reliefos/config/root.cfg", "reliefos/state/kernel-A.env", "grub/grub.cfg")
        def first_state():
            return {p: hashlib.sha256(esp_bytes(disk, esp, f"reliefos/instances/{identities['first']}/{p}")).hexdigest() for p in members}
        first_files = first_state()
        previous_check = checked_fixture(args.checked_fixture.resolve(), identities) if args.checked_fixture else None
        if not previous_check:
            seed_second_fixture(args.root_fixture.resolve(), disk, esp, identities, output)
        library = args.worker_library.resolve() if args.worker_library else None
        worker = worker_disk(args.worker_binary.resolve(), output, library) if args.worker_binary else None
        with boot(output / "update-two-roots", "bios", disk=disk, extra_disk=worker,
                  iso=args.images.resolve() / "reliefos-installer.iso") as session:
            enter_console(*session)
            if worker:
                probe, serial, process = session
                probe.text("mkdir -p /mnt/probe; mount -t vfat -o ro /dev/sdb1 /mnt/probe; cp /mnt/probe/installer.elf /tmp/installer; chmod 755 /tmp/installer; export PATH=/tmp:$PATH; echo __WORKER_READY_$? > /dev/ttyS0")
                probe.key("ret")
                wait_for(serial, process, lambda text: "__WORKER_READY_0" in text, "current native worker", args.timeout)
                probe.text("sha256sum /tmp/installer > /dev/ttyS0")
                probe.key("ret")
                expected_worker = hashlib.sha256(args.worker_binary.read_bytes()).hexdigest()
                wait_for(serial, process, lambda text: expected_worker in text,
                         "exact current installer executable checksum", args.timeout)
                if library:
                    probe.text("cp /mnt/probe/libreliefos.so.2 /tmp/libreliefos.so.2; export LD_LIBRARY_PATH=/tmp; sha256sum /tmp/libreliefos.so.2 > /dev/ttyS0")
                    probe.key("ret")
                    expected_library = hashlib.sha256(library.read_bytes()).hexdigest()
                    wait_for(serial, process, lambda text: expected_library in text,
                             "exact current runtime library checksum", args.timeout)
            if previous_check:
                apply_checked_update(*session, previous_check, args.install_timeout)
            else:
                check_update_selection(*session, identities, args.install_timeout, after_install=False)
        assert_foreign(disk, esp)
        assert first_state() == first_files, "Second-root update changed the first installation"
    chooser = efi_chooser(output, disk, esp, identities["first"])
    for firmware, identity in (("bios", identities["second"]), ("uefi", identities["first"])):
        working = output / f"boot-{firmware}.raw"
        run("cp", "--reflink=auto", "--sparse=always", str(disk), str(working))
        with boot(output / f"boot-{firmware}", firmware, disk=working,
                  extra_disk=chooser if firmware == "uefi" else None,
                  boot_extra=firmware == "uefi") as (probe, serial, process):
            wait_for(serial, process, lambda text: boot_evidence(text, firmware) and identity in text,
                     "selected installation root and init", args.timeout)
            wait_for(serial, process, lambda text: f"[initramfs] root=/dev/disk/by-partuuid/{identity} " in text,
                     "selected real root with its boot directory mounted", args.timeout)
            if args.first_installation and identity == identities["second"]:
                wait_xdm(probe, process, "updated-xdm", args.timeout)
            else:
                wait_text(probe, process, "console-login", "login:", timeout=args.timeout)
            probe.frame("boot-complete")
        print(f"PASS actual {firmware} boot of {identity}", flush=True)
    selected = output / "boot-other-instance.raw"
    run("cp", "--reflink=auto", "--sparse=always", str(disk), str(selected))
    with boot(output / "boot-other-instance", "bios", disk=selected) as (probe, serial, process):
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            probe.key("up")
            frame = probe.frame("other-installation-menu")
            menu = frame.crop((frame.width // 2, frame.height // 4, frame.width, frame.height * 3 // 4))
            if "advanced options" in ocr(menu).lower():
                break
        else:
            raise AssertionError("GRUB did not enumerate the other installation")
        probe.key("down"); probe.key("down"); probe.key("ret")
        # Selecting another GRUB config opens its own menu; let that default boot.
        wait_for(serial, process, lambda text: boot_evidence(text, "bios") and identities["first"] in text,
                 "another installation selected through the GRUB menu", args.timeout)
        wait_for(serial, process, lambda text: f"[initramfs] root=/dev/disk/by-partuuid/{identities['first']} " in text,
                 "the other instance's real root and boot directory", args.timeout)
        wait_text(probe, process, "other-console-login", "login:", timeout=args.timeout)
        probe.frame("other-installation-booted")
    print("PASS GRUB menu selection of the other installation", flush=True)
    assert_foreign(disk, esp)
    (output / "result.json").write_text(json.dumps({"passed": True, "identities": identities,
        "foreign_efi_preserved": True, "first_boot_state_preserved": True}, indent=2) + "\n")


if __name__ == "__main__":
    main()
