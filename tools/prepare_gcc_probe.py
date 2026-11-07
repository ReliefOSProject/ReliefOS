#!/usr/bin/env python3
"""Stage an isolated ext2 boot disk with an unchanged prebuilt musl GCC."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--toolchain", required=True, type=Path,
                        help="Explicit external GCC fixture root from package_musl_gcc.py")
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    stage = out / "stage"
    if stage.exists():
        shutil.rmtree(stage)
    shutil.copytree(ROOT / "build/esp", stage, symlinks=True)
    for namespace in ("reliefos", "leonos"):
        (stage / namespace).mkdir(exist_ok=True)
        shutil.copy2(ROOT / "build/system/kernel.sys", stage / namespace / "kernel.sys")
    from package_musl_gcc import COMMANDS
    for name in ("opt/dyne", "usr/share/licenses/musl-gcc", "usr/share/examples/musl-gcc"):
        shutil.copytree(args.toolchain / name, stage / name, symlinks=True)
    for name in COMMANDS:
        link = stage / "usr/bin" / name
        link.unlink(missing_ok=True)
        link.symlink_to("../../opt/dyne/bin/reliefos-musl-cc")
    tests = stage / "usr/lib/reliefos/tests"
    tests.mkdir(parents=True, exist_ok=True)
    shutil.copy2(args.runner, tests / "gcc-probe.elf")
    config = stage / "grub/grub.cfg"
    config.write_text("""set timeout=0
set default=0
insmod part_gpt
insmod fat
insmod multiboot2
insmod all_video
set gfxpayload=keep
menuentry "ReliefOS GCC ABI probe" {
    set root=(hd0,gpt1)
    multiboot2 /reliefos/kernel.sys root=/ log=serial autospawn=gcc syscall-trace=/opt/dyne/
    boot
}
""", encoding="ascii")
    hashes = {}
    for name in ("gcc-musl/bin/x86_64-linux-musl-gcc",
                 "gcc-musl/libexec/gcc/x86_64-linux-musl/15.1.0/cc1",
                 "gcc-musl/bin/x86_64-linux-musl-as", "gcc-musl/bin/x86_64-linux-musl-ld"):
        with (stage / "opt/dyne" / name).open("rb") as stream:
            hashes[name] = hashlib.file_digest(stream, "sha256").hexdigest()
    (out / "binary-sha256.json").write_text(json.dumps(hashes, indent=2) + "\n")
    subprocess.run(["python3", "tools/make_image.py", "--esp-tree", str(stage),
        "--size-mib", "1024", "--root-fs", "ext2", "--out", str(out / "gcc-probe.vmdk"),
        "--raw", str(out / "gcc-probe.raw"), "--esp-image", str(out / "esp.fat"),
        "--root-image", str(out / "root.ext2")], cwd=ROOT, check=True)


if __name__ == "__main__":
    main()
