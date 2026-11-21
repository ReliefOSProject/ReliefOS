#!/usr/bin/env python3
"""Compatibility arguments for the native, shared BIOS/UEFI ISO producer."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    """Stage private inputs; delegate all firmware/image formats to native rules."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-output", type=Path, default=ROOT / "out/x86_64/release")
    parser.add_argument("--out", type=Path)
    parser.add_argument("--stage", type=Path)
    parser.add_argument("--boot-image", type=Path, help="Optional extracted EFI boot image for old callers")
    parser.add_argument("--kernel", type=Path)
    parser.add_argument("--initramfs", type=Path)
    parser.add_argument("--installer-root", type=Path)
    parser.add_argument("--installer-payload", type=Path,
                        help="Default: the build's SquashFS payload unless --installer-root is explicit")
    parser.add_argument("--grub-font", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--grub-efi-dir", type=Path)
    parser.add_argument("--grub-bios-dir", type=Path)
    parser.add_argument("--grub-mkimage", type=Path)
    parser.add_argument("--grub-mkstandalone", type=Path)
    parser.add_argument("--grub-config", type=Path, default=ROOT / "boot/grub/installer.cfg")
    parser.add_argument("--bios", action="store_true", help="Compatibility option; both platforms are always included")
    args = parser.parse_args()
    output = args.build_output.resolve()
    iso = (args.out or output / "images/reliefos-installer.iso").resolve()
    kernel = (args.kernel or output / "generated/system/kernel.sys").resolve()
    initramfs = (args.initramfs or output / "generated/system/initramfs.cpio").resolve()
    root = (args.installer_root or output / "images/installer-root.ext4").resolve()
    payload = args.installer_payload
    if payload is None and args.installer_root is None:
        payload = output / "images/installer-payload.squashfs"
    if payload is not None:
        payload = payload.resolve()
    font = (args.grub_font or output / "generated/grub/leonos-unicode.pf2").resolve()
    efi = (args.grub_efi_dir or output / "grub-efi/lib/grub/x86_64-efi").resolve()
    bios = (args.grub_bios_dir or output / "grub-host/lib/grub/i386-pc").resolve()
    mkimage = (args.grub_mkimage or output / "grub-host/bin/grub-mkimage").resolve()
    standalone = (args.grub_mkstandalone or output / "grub-host/bin/grub-mkstandalone").resolve()
    required_inputs = [kernel, initramfs, Path(str(initramfs) + ".json"), root,
                       font, args.grub_config, efi / "kernel.img", bios / "kernel.img",
                       mkimage, standalone]
    if payload:
        required_inputs.append(payload)
    for item in required_inputs:
        if not item.is_file():
            parser.error(f"missing explicit build input {item}; build grub-host grub-efi esp first")
    parent = (args.work_dir or output / "tmp").resolve()
    parent.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ, GRUB_MKSTANDALONE=str(standalone))
    epoch = environment.get("SOURCE_DATE_EPOCH", "0")
    with tempfile.TemporaryDirectory(prefix="dual-iso-", dir=parent) as directory:
        work = Path(directory)
        stage = (args.stage or work / "esp").resolve()
        staged_inputs = [iso, kernel, initramfs, root, font, args.grub_config.resolve()]
        if payload is not None:
            staged_inputs.append(payload)
        if any(item.is_relative_to(stage) for item in staged_inputs):
            parser.error("stage must not contain source inputs or the published ISO")
        display = work / "display.conf"
        display.write_text("theme=metro\nwallpaper.mode=fill\n")
        subprocess.run(["sh", ROOT / "tools/build/boot-stage.sh", ROOT, efi, bios,
                        kernel, initramfs, font, display, stage, epoch], env=environment, check=True)
        iso_command = ["sh", ROOT / "tools/build/iso.sh", ROOT, efi, stage, root,
                       args.grub_config.resolve(), iso, epoch, "RELIEFOSINST", bios, mkimage]
        if payload:
            iso_command.append(payload)
        subprocess.run(iso_command, env=environment, check=True)
    if args.boot_image:
        args.boot_image.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["xorriso", "-osirrox", "on", "-indev", iso,
                        "-extract", "/boot/efiboot.img", args.boot_image.resolve()], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
