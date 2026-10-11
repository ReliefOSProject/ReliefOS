#!/usr/bin/env python3
from __future__ import annotations

import argparse
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GRUB_MODULES = (
    "part_gpt fat iso9660 multiboot2 normal search search_fs_file configfile echo serial "
    "terminal video video_bochs video_cirrus efi_gop efi_uga all_video font gfxterm gfxmenu"
)


def run(cmd: list[str]) -> None:
    print("+", " ".join(cmd))
    subprocess.run(cmd, cwd=ROOT, check=True)


def copy_file(src: Path, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def build_installer_boot_efi(out: Path, grub_efi_dir: Path) -> Path:
    out.parent.mkdir(parents=True, exist_ok=True)
    run([
        "grub-mkstandalone",
        "-d",
        str(grub_efi_dir),
        "-O",
        "x86_64-efi",
        "-o",
        str(out),
        f"--modules={GRUB_MODULES}",
        "boot/grub/grub.cfg=boot/grub/installer_embedded.cfg",
    ])
    return out


def stage_installer_tree(
    stage: Path,
    boot_image: Path,
    boot_efi: Path,
    kernel: Path,
    installer_root: Path,
    grub_font: Path,
    grub_config: Path,
) -> None:
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)
    copy_file(boot_efi, stage / "EFI/BOOT/BOOTX64.EFI")
    copy_file(grub_config, stage / "grub/grub.cfg")
    copy_file(grub_font, stage / "grub/fonts/leonos-unicode.pf2")
    copy_file(ROOT / "boot/grub/theme/theme.txt", stage / "grub/theme/theme.txt")
    for namespace in ("reliefos", "leonos"):
        (stage / f"{namespace}-installer-iso.marker").write_text("ReliefOS installer ISO volume\n", encoding="ascii")
        copy_file(kernel, stage / namespace / "kernel.sys")
    copy_file(installer_root, stage / "install/root.fat")
    copy_file(boot_image, stage / "boot/efiboot.img")


def create_boot_image(boot_image: Path, boot_efi: Path, boot_stage: Path) -> None:
    if boot_stage.exists():
        shutil.rmtree(boot_stage)
    boot_stage.mkdir(parents=True)
    copy_file(boot_efi, boot_stage / "EFI/BOOT/BOOTX64.EFI")

    payload_bytes = sum(item.stat().st_size for item in boot_stage.rglob("*") if item.is_file())
    required_mib = (payload_bytes + (1024 * 1024 - 1)) // (1024 * 1024)
    size_mib = required_mib + 8
    if size_mib < 16:
        size_mib = 16

    boot_image.parent.mkdir(parents=True, exist_ok=True)
    if boot_image.exists():
        boot_image.unlink()
    run(["truncate", "-s", f"{size_mib}M", str(boot_image)])
    run(["mkfs.fat", "-F", "16", "-n", "RELIEFOS", str(boot_image)])
    for item in sorted(boot_stage.iterdir()):
        run(["mcopy", "-s", "-i", str(boot_image), str(item), "::/"])


def main() -> int:
    parser = argparse.ArgumentParser(description="Create ReliefOS UEFI installer ISO")
    parser.add_argument("--out", default="build/images/reliefos-installer.iso")
    parser.add_argument("--stage", default="build/installer-iso")
    parser.add_argument("--boot-image", default="build/install/installer-efiboot.img")
    parser.add_argument("--kernel", default="build/system/kernel.sys")
    parser.add_argument("--installer-root", default="build/install/root.fat")
    parser.add_argument("--grub-font", default="build/generated/grub/leonos-unicode.pf2")
    parser.add_argument("--work-dir", default="build/install")
    parser.add_argument("--grub-efi-dir", default="/usr/lib/grub/x86_64-efi")
    parser.add_argument("--grub-config", default="boot/grub/installer.cfg")
    parser.add_argument("--bios", action="store_true", help="Also include the BIOS GRUB boot path")
    args = parser.parse_args()

    out = ROOT / args.out
    stage = ROOT / args.stage
    boot_image = ROOT / args.boot_image
    kernel = ROOT / args.kernel
    installer_root = ROOT / args.installer_root
    grub_font = ROOT / args.grub_font
    work_dir = ROOT / args.work_dir
    grub_efi_dir = Path(args.grub_efi_dir)
    if not grub_efi_dir.is_absolute():
        grub_efi_dir = ROOT / grub_efi_dir

    out.parent.mkdir(parents=True, exist_ok=True)
    if out.exists():
        out.unlink()

    boot_efi = build_installer_boot_efi(work_dir / "installer-BOOTX64.EFI", grub_efi_dir)
    create_boot_image(boot_image, boot_efi, work_dir / "efi-boot")
    stage_installer_tree(stage, boot_image, boot_efi, kernel,
                         installer_root, grub_font, ROOT / args.grub_config)
    if args.bios:
        copy_file(ROOT / args.grub_config, stage / "boot/grub/grub.cfg")
        bios_dir = Path("/usr/lib/grub/i386-pc")
        core = work_dir / "bios-core.img"
        run(["grub-mkimage", "-d", str(bios_dir), "-O", "i386-pc", "-p", "/boot/grub",
             "-o", str(core), "biosdisk", "iso9660", "normal", "configfile", "multiboot2",
             "search", "search_fs_file", "serial", "terminal", "all_video", "font", "gfxterm"])
        # Keep the known EFI standalone image; the host grub-mkrescue EFI
        # build may differ from the repository's validated GRUB modules.
        (stage / "boot/grub/eltorito.img").write_bytes((bios_dir / "cdboot.img").read_bytes() + core.read_bytes())
    run([
        "xorriso",
        "-as",
        "mkisofs",
        "-iso-level",
        "3",
        "-R",
        "-J",
        "-V",
        "RELIEFOSINST",
        *(("-b", "boot/grub/eltorito.img", "-no-emul-boot", "-boot-load-size", "4",
           "-boot-info-table", "-eltorito-alt-boot") if args.bios else ()),
        "-e",
        "boot/efiboot.img",
        "-no-emul-boot",
        "-o",
        str(out),
        str(stage),
    ])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
