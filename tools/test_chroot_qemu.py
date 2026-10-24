#!/usr/bin/env python3
"""Run the same chroot probe on Linux and ReliefOS, including dynamic exec."""
from pathlib import Path
import shutil
import subprocess
import tempfile

import test_apk_qemu as runner
from make_ext2_root import write_ext2_root
from make_live_root import make_live_tree

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / "build/chroot-qemu"


def fixture(path, dynamic):
    for name in ("bin", "lib", "nested"):
        (path / name).mkdir(parents=True, exist_ok=True)
    (path / "marker").write_text("inside")
    (path / "absolute").symlink_to("/marker")
    (path / "relative").symlink_to("../../marker")
    shutil.copy2(dynamic, path / "bin/probe")
    shutil.copy2(ROOT / "build/musl/sysroot/lib/libc.so", path / "lib/ld-musl-x86_64.so.1")
    shutil.copy2(ROOT / "build/musl/sdk/lib/libmimalloc.so.3", path / "lib/libmimalloc.so.3")


def main():
    WORK.mkdir(parents=True, exist_ok=True)
    compiler = ROOT / "build/musl/sdk/bin/reliefos-musl-cc"
    source = ROOT / "kernel/reliefnt/tools/tests/chroot_runtime_probe.c"
    for name, flags in (("static", ["-static"]), ("dynamic", ["-Wl,--as-needed"])):
        subprocess.run([str(compiler), "-O2", *flags, str(source), "-o", str(WORK / name)], check=True)
    with tempfile.TemporaryDirectory(prefix="reference-", dir=WORK) as tmp:
        fixture(Path(tmp), WORK / "dynamic")
        subprocess.run(["unshare", "-Ur", str(WORK / "static"), tmp], check=True)
    with tempfile.TemporaryDirectory(prefix="stage-", dir=WORK) as tmp:
        stage = Path(tmp) / "root"
        make_live_tree(ROOT / "build/apk/root", stage)
        fixture(stage / "tmp/chroot-fixture", WORK / "dynamic")
        tests = stage / "usr/lib/reliefos/tests"
        tests.mkdir(parents=True, exist_ok=True)
        shutil.copy2(WORK / "static", tests / "chroot-probe")
        launcher = WORK / "launcher.c"
        launcher.write_text('#include <unistd.h>\nint main(void) {\n'
                            'execl("/usr/lib/reliefos/tests/chroot-probe", "probe", '
                            '"/tmp/chroot-fixture", (char *)0); return 1; }\n')
        subprocess.run([str(compiler), "-static", str(launcher), "-o",
                        str(tests / "linux-inventory.elf")], check=True)
        write_ext2_root(stage, WORK / "root.ext2", minimum_mib=512)
    iso = runner.iso_tools
    iso.GRUB_TEMPLATE = iso.GRUB_TEMPLATE.replace(
        "autospawn=ioctlcloexec autospawn=python315", "autospawn=inventory").replace(
        "syscall-trace=/opt/python/", "").replace("set timeout=5", "set timeout=0")
    iso.build_iso(WORK / "root.ext2", WORK / "reliefos-apk.iso", WORK / "grub.cfg", WORK)
    runner.WORK = WORK
    runner.guest(180)


if __name__ == "__main__":
    main()
