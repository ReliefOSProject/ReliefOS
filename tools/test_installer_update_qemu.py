#!/usr/bin/env python3
"""Update an isolated copy of an installed test disk through the actual GUI."""
import argparse
from pathlib import Path
import subprocess
import re
import json
import hashlib
import os

from test_installer_accounts_qemu import (
    boot, next_page, select_installation_mode, installer_finish_log,
)
from test_installer_mode_switch_qemu import wait_page

ROOT = Path(__file__).resolve().parents[1]


def installed_state(disk, directory):
    table = json.loads(subprocess.check_output(["sfdisk", "--json", str(disk)], text=True))["partitiontable"]
    partition = next(part for part in table["partitions"]
                     if part["type"].lower() == "0fc63daf-8483-4772-8e79-3d69d8477de4")
    image = directory / "target-root.ext2"
    subprocess.run(["dd", f"if={disk}", f"of={image}", "bs=1M", "iflag=skip_bytes,count_bytes",
                    f"skip={partition['start'] * table['sectorsize']}",
                    f"count={partition['size'] * table['sectorsize']}", "conv=sparse", "status=none"], check=True)
    result = {"partitiontable": table}
    for name, path in (("world", "/etc/apk/world"), ("installed", "/lib/apk/db/installed")):
        value = subprocess.check_output(["debugfs", "-R", f"cat {path}", str(image)],
                                        stderr=subprocess.DEVNULL, text=True)
        assert value, f"Missing {path}"
        result[name] = value
    result["checkpoint"] = subprocess.check_output([
        "debugfs", "-R", "cat /var/lib/reliefos/installer-update/state", str(image)],
        stderr=subprocess.DEVNULL, text=True)
    result["preserved"] = {}
    for path in ("/etc/passwd", "/etc/shadow", "/etc/group", "/etc/gshadow", "/etc/fstab",
                 "/etc/hostname", "/home/alice/installer-user-check", "/root/installer-root-check"):
        content = subprocess.check_output(["debugfs", "-R", f"cat {path}", str(image)],
                                          stderr=subprocess.DEVNULL)
        if content:
            result["preserved"][path] = hashlib.sha256(content).hexdigest()
    required = {"/etc/passwd", "/etc/shadow", "/etc/group", "/etc/gshadow", "/etc/fstab", "/etc/hostname"}
    assert required <= result["preserved"].keys(), "Missing installed account or filesystem configuration"
    image.unlink()
    return result


def package_versions(database):
    versions = {}
    for entry in database.split("\n\n"):
        fields = dict(line.split(":", 1) for line in entry.splitlines()
                      if line.startswith(("P:", "V:")))
        if "P" in fields:
            versions[fields["P"]] = fields["V"]
    return versions


def validate_versions(before, after, allow_same_version=False):
    if allow_same_version:
        expected = {re.sub(r"^leonos-", "reliefos-", name): version
                    for name, version in before.items()}
        assert len(expected) == len(before), "Ambiguous legacy/canonical package names"
        assert expected == after, "Same-version repair changed installed package versions"
    else:
        assert any(name.startswith(("leonos-", "reliefos-")) and
                   after.get(re.sub(r"^leonos-", "reliefos-", name)) not in (None, version)
                   for name, version in before.items()), "No ReliefOS package was upgraded"


def main():
    os.environ.setdefault("OMP_THREAD_LIMIT", "1")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disk", type=Path, required=True, help="Read-only input; only its copy is updated")
    parser.add_argument("--iso", type=Path,
                        default=ROOT / "out/x86_64/release/images/reliefos-installer.iso")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=2400)
    parser.add_argument("--preserve-package", action="append", default=[])
    parser.add_argument("--allow-same-version", action="store_true",
                        help="Exercise an equal-version repair; require all package versions to remain equal")
    parser.add_argument("--accel", choices=("auto", "kvm", "tcg"), default="auto")
    parser.add_argument("--firmware", choices=("bios", "uefi"), default="uefi")
    parser.add_argument("--firmware-image", type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    disk = output / "scratch.raw"
    if disk.exists():
        parser.error("output scratch.raw already exists; use a new output directory")
    subprocess.run(["cp", "--reflink=auto", "--sparse=always", args.disk.resolve(), disk], check=True)
    before = installed_state(disk, output)
    versions = package_versions(before["installed"])
    if args.preserve_package:
        assert all(name in versions for name in args.preserve_package), "Fixture lacks required packages"
    from test_installer_flow_qemu import poweroff, wait_gui_startup
    with boot(output / "update", disk, args.iso.resolve(), accel=args.accel,
              firmware=args.firmware, firmware_image=args.firmware_image) as (probe, serial, process):
        wait_gui_startup(probe, serial, process, min(args.timeout, 600))
        probe.click(*probe.language_option_point())
        wait_page(probe, process, "Language")
        probe.frame("language")
        for page in (1, 3):
            next_page(probe, page)
        select_installation_mode(probe, update=True)
        next_page(probe, 4)
        probe.click(*probe.disk_selection_point())
        try:
            probe.click(*probe.footer_point())
            wait_page(probe, process, "Review update", args.timeout)
            assert probe.visible_page(probe.frame("inspection-complete")) == 4
            next_page(probe, 8)
            wait_page(probe, process, "Review system update", args.timeout)
            probe.frame("confirmation")
            next_page(probe)
            wait_page(probe, process, "was updated on the selected disk", args.timeout)
            probe.frame("updated")
        finally:
            (output / "installer.log").write_text(installer_finish_log(
                probe, serial, process, log_path="/var/log/installer-update.log"))
        log = (output / "installer.log").read_text()
        assert "update completed successfully" in log
        poweroff(probe, process)
    after = installed_state(disk, output)
    assert after["partitiontable"] == before["partitiontable"], "Update changed the installed GPT"
    assert "\nphase=done\n" in after["checkpoint"], "The native update transaction did not finish"
    for path, digest in before["preserved"].items():
        assert after["preserved"].get(path) == digest, f"Update changed preserved file {path}"
    print("PASS preserved accounts, filesystem configuration and existing test user files")
    updated = package_versions(after["installed"])
    expected_world = re.sub(r"(?m)^leonos-", "reliefos-", before["world"])
    # apk serializes world in lexical order after renaming dependencies.
    assert sorted(expected_world.splitlines()) == sorted(after["world"].splitlines()), \
        "Update changed world dependencies beyond the package rename"
    validate_versions(versions, updated, args.allow_same_version)
    if args.preserve_package:
        for name in args.preserve_package:
            assert versions[name] == updated.get(name), f"Update changed {name}"
        print("PASS preserved world and packages: " + ", ".join(args.preserve_package))
    (output / "update-evidence.json").write_text(json.dumps({
        "same_version": args.allow_same_version, "phase": "done",
        "preserved_files": before["preserved"], "package_count": len(updated),
        "versions_verified": True, "firmware": args.firmware,
    }, indent=2) + "\n")
    print(f"PASS GUI update: confirmation, package transaction and completion; {serial}")


if __name__ == "__main__":
    main()
