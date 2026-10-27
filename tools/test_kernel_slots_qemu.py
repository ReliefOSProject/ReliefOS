#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Execute the real GRUB slot selector on private BIOS/UEFI ISO fixtures.

The payloads are inert byte strings: no kernel, initramfs or OS is executed.
This complements the environment codec oracle with actual firmware execution,
including a checksum-list omission combined with a damaged omitted payload.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from test_dual_boot_qemu import boot, wait_for

sys.path.insert(0, str(Path(__file__).resolve().parent / "tests"))
from kernel_slot_oracle import checksum, encode, write_version

ROOT = Path(__file__).resolve().parents[1]


def publish(tree, slot, generation):
    """Create an independent canonical slot with inert, distinct payloads."""
    kernel = b"ReliefOS GRUB-only kernel fixture " + bytes([generation])
    archive = b"ReliefOS GRUB-only initramfs fixture " + bytes([generation])
    fields = dict(generation=generation, release=f"5.1.2-ko1-{generation:012x}",
                  module_abi=f"{generation:012x}" + "0" * 52,
                  kernel_sha256=hashlib.sha256(kernel).hexdigest(),
                  initramfs_sha256=hashlib.sha256(archive).hexdigest())
    directory = write_version(tree, fields, kernel, archive)
    state = tree / "reliefos/state"
    state.mkdir(exist_ok=True, parents=True)
    record = encode(fields)
    (state / f"kernel-{slot}.env").write_bytes(record)
    (state / f"kernel-{slot}.sha256").write_bytes(checksum(slot, record))
    return directory


def hash_checks():
    """Require exact comparisons and rejection of malformed or mixed modes."""
    expected = hashlib.sha256(b"GRUB explicit digest fixture\n").hexdigest()
    prefix = "hashsum --hash sha256 "
    cases = [
        (True, f"--expected {expected} /digest.txt"),
        (True, f"--expected {expected.upper()} /digest.txt"),
        (False, "--expected " + "0" * 64 + " /digest.txt"),
        (False, "--expected abc /digest.txt"),
        (False, "--expected " + "z" * 64 + " /digest.txt"),
        (False, f"--expected {expected} /missing-file"),
        (False, f"--expected {expected}"),
        (False, f"--expected {expected} /digest.txt /digest.txt"),
        (False, f"--expected {expected} --uncompress /digest.txt"),
        (False, f"--expected {expected} --keep-going /digest.txt"),
        (False, f"--expected {expected} --prefix / /digest.txt"),
        (False, f"--expected {expected} --check /digest.sha256"),
        (True, "--check /digest.sha256"),
    ]
    lines = ["insmod hashsum", "insmod gcry_sha256"]
    for number, (success, arguments) in enumerate(cases):
        yes, no = ("PASS", "FAIL") if success else ("FAIL", "PASS")
        lines.append(f"if {prefix}{arguments}; then echo QA_HASH_{number}={yes}; "
                     f"else echo QA_HASH_{number}={no}; fi")
    return lines, len(cases)


def make_iso(output, stage, selector, case):
    """Use the production GNU GRUB tools/modules and a private ordinary ISO."""
    output.mkdir()
    tree = output / "tree"
    grub = tree / "grub"
    grub.mkdir(parents=True)
    pc = stage / "grub-host/lib/grub/i386-pc"
    efi = stage / "grub-efi/lib/grub/x86_64-efi"
    shutil.copytree(pc, grub / "i386-pc")
    shutil.copytree(efi, grub / "x86_64-efi")
    shutil.copyfile(selector, grub / "kernel-slots.cfg")
    directory = publish(tree, "A", 1)
    chosen, ready = "A", 1
    if case == "fallback":
        directory = publish(tree, "B", 2)
    if case != "valid":
        sums = directory / "SHA256SUMS"
        sums.write_bytes(b"".join(line + b"\n" for line in sums.read_bytes().splitlines()
                                 if line.endswith(b"  boot.env")))
        member = "initramfs.cpio" if case == "omitted-initramfs" else "kernel.sys"
        payload = directory / member
        data = bytearray(payload.read_bytes())
        data[len(data) // 2] ^= 1
        payload.write_bytes(data)
        if case != "fallback":
            chosen, ready = "", 0
    (tree / "reliefos-ko-grub-fixture.marker").write_text("GRUB-only owned QA fixture\n")
    (tree / "digest.txt").write_bytes(b"GRUB explicit digest fixture\n")
    (tree / "digest.sha256").write_text(
        hashlib.sha256((tree / "digest.txt").read_bytes()).hexdigest() + "  /digest.txt\n")
    checks, count = hash_checks() if case == "valid" else ([], 0)
    config = ["serial --unit=0 --speed=115200", "terminal_output serial", *checks,
              "source ($root)/grub/kernel-slots.cfg",
              "echo QA_SLOT_READY=$reliefos_boot_ready",
              "echo QA_SLOT_SELECTED=$reliefos_selected", "echo QA_GRUB_DONE", "insmod halt", "halt"]
    (grub / "grub.cfg").write_text("\n".join(config) + "\n")
    embedded = output / "embedded.cfg"
    embedded.write_text("search --file --set=root /reliefos-ko-grub-fixture.marker\n"
                        "configfile ($root)/grub/grub.cfg\n")
    efi_tree = output / "efi"
    loader = efi_tree / "EFI/BOOT/BOOTX64.EFI"
    loader.parent.mkdir(parents=True)
    tools = stage / "grub-host/bin"
    with (output / "build.log").open("xb") as log:
        def run(command):
            subprocess.run([str(x) for x in command], check=True, stdout=log, stderr=subprocess.STDOUT)
        modules = (ROOT / "boot/grub/bios-modules.list").read_text().split()
        run([tools / "grub-mkimage", "-O", "i386-pc-eltorito", "-d", pc, "-p", "/grub",
             "-c", embedded, "-o", tree / "bios.img", *modules])
        run([tools / "grub-mkstandalone", "-d", efi, "-O", "x86_64-efi", "-o", loader,
             "--modules=part_gpt fat iso9660 normal search search_fs_file configfile serial terminal",
             "boot/grub/grub.cfg=" + str(embedded)])
        fat = tree / "efiboot.img"
        with fat.open("xb") as stream:
            stream.truncate(16 * 1024**2)
        run(["mkfs.fat", "--invariant", "-F", "16", fat])
        run(["mcopy", "-s", "-i", fat, efi_tree / "EFI", "::/"])
        iso = output / "fixture.iso"
        run(["xorriso", "-as", "mkisofs", "-R", "-b", "bios.img", "-no-emul-boot",
             "-boot-load-size", "4", "-boot-info-table", "--grub2-boot-info",
             "-eltorito-alt-boot", "-e", "efiboot.img", "-no-emul-boot", "-o", iso, tree])
    return iso, ready, chosen, count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, required=True, help="native O with built GRUB platforms")
    parser.add_argument("--selector", type=Path, default=ROOT / "boot/grub/kernel-slots.cfg")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not os.environ.get("TMPDIR") or not Path(os.environ["TMPDIR"]).is_absolute():
        raise ValueError("set an absolute project TMPDIR")
    args.output.mkdir()
    results = []
    for case in ("valid", "omitted-kernel", "omitted-initramfs", "fallback"):
        case_dir = args.output / case
        iso, ready, selected, count = make_iso(case_dir, args.stage.resolve(), args.selector.resolve(), case)
        for firmware in ("bios", "uefi"):
            with boot(case_dir / firmware, firmware, iso=iso) as (probe, serial, process):
                text = wait_for(serial, process, lambda s: "QA_GRUB_DONE" in s,
                                "real GRUB self-test completion", 60)
                assert f"QA_SLOT_READY={ready}" in text, text
                if ready:
                    assert f"QA_SLOT_SELECTED={selected}" in text, text
                assert "=FAIL" not in text, text
                assert all(f"QA_HASH_{n}=PASS" in text for n in range(count)), text
            results.append(dict(case=case, firmware=firmware, passed=True, hash_cases=count))
            (args.output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
            print("PASS", firmware, case, flush=True)


if __name__ == "__main__":
    main()
