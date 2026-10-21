#!/usr/bin/env python3
"""Run Alpine Clang on the ReliefNT kernel.

Clang is the stress case for the user address space: musl's ldso reserves one
whole PT_LOAD span per shared object with a single mmap, and Alpine's
libLLVM.so.22.1 spans 183 MiB.  With RELIEFNT_USER_TOP at 512 MiB the read-only
file mmap arena was 255 MiB, Clang's 278 MiB DT_NEEDED closure did not fit,
mmap returned ENOMEM, and the 2500 "Error relocating /usr/bin/clang:
LLVMInitialize*Target: symbol not found" lines were only the downstream
cascade.  This fixture pins the arena and the successful end-to-end run.

The fixture is deliberately separate from test_alpine_runtime_qemu.py so the
GCC runtime baseline stays untouched.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

from apk_distribution import ROOT, bootstrap, run
from make_live_root import make_live_tree
from make_ext2_root import write_ext2_root
import test_linux_ioctl_cloexec as iso_tools

WORK = ROOT / "build/clang-runtime"
# clang provides the compiler, gcc provides crtbeginS.o and the bfd linker that
# its driver invokes, and reliefos-musl-dev provides the headers and libc.a.
PACKAGES = ["clang", "gcc", "reliefos-musl-dev"]


def layout():
    """Fail before booting when the checked-out kernel cannot address libLLVM.

    build_iso() consumes a prebuilt build/system/kernel.sys, so a stale kernel
    image could otherwise boot a Clang fixture that no longer exercises the
    user address space the guest probe is here to measure.
    """
    with tempfile.TemporaryDirectory(prefix="clang-layout-") as directory:
        executable = Path(directory) / "user_mmap_arena_test"
        run(["cc", "-std=c11", "-O1", "-Wall", "-Wextra",
             "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/include",
             ROOT / "tools/tests/user_mmap_arena_test.c", "-o", executable])
        run([executable])


def prepare(packages, refresh=False):
    apk = bootstrap()
    managed = ROOT / "build/apk/root"
    signature = (ROOT / "build/apk/normal/manifest.json").read_text()
    root = WORK / "root"
    stamp = WORK / "source.json"
    expected = {"source": signature, "packages": packages}
    cache = WORK / "cache"
    cache.mkdir(parents=True, exist_ok=True)
    if refresh or not stamp.is_file() or json.loads(stamp.read_text()) != expected:
        if not (managed / "lib/apk/db/installed").is_file():
            raise RuntimeError("run python3 build.py run apk-root first")
        with tempfile.TemporaryDirectory(prefix="prepare-", dir=WORK) as directory:
            stage = Path(directory) / "root"
            shutil.copytree(managed, stage, symlinks=True)
            base = ["unshare", "-Ur", apk, "--root", stage,
                    "--repositories-file", ROOT / "system/rootfs/etc/apk/repositories",
                    "--repository", stage / "usr/share/reliefos/apk/repository/packages.adb",
                    "--cache-dir", cache, "--cache-packages", "--timeout", "120"]
            run([*base, "update"])
            run([*base, "add", "--upgrade", *packages])
            for binary in ("/usr/bin/clang", "/lib/ld-musl-x86_64.so.1"):
                if not (stage / binary.lstrip("/")).is_file():
                    raise RuntimeError(f"clang fixture is missing {binary}")
            if root.exists():
                shutil.rmtree(root)
            shutil.move(stage, root)
            stamp.write_text(json.dumps(expected))
    sysroot = ROOT / "build/musl/sysroot"
    run(["clang", "--target=x86_64-linux-musl", "-fuse-ld=lld", "-nostdlib", "-nostdinc",
         "-isystem", sysroot / "include", "-static", "-O2", "-Wall", "-Wextra",
         sysroot / "lib/crt1.o", sysroot / "lib/crti.o", ROOT / "tools/tests/clang_guest_probe.c",
         sysroot / "lib/libc.a", sysroot / "lib/crtn.o", "-o", WORK / "probe.elf"])
    with tempfile.TemporaryDirectory(prefix="image-", dir=WORK) as directory:
        stage = Path(directory) / "root"
        make_live_tree(root, stage)
        tests = stage / "usr/lib/reliefos/tests"
        tests.mkdir(parents=True, exist_ok=True)
        shutil.copy2(WORK / "probe.elf", tests / "linux-inventory.elf")
        write_ext2_root(stage, WORK / "root.ext2", minimum_mib=64)
    iso_tools.GRUB_TEMPLATE = iso_tools.GRUB_TEMPLATE.replace(
        "autospawn=ioctlcloexec autospawn=python315", "autospawn=inventory").replace(
        "syscall-trace=/opt/python/", "").replace("set timeout=5", "set timeout=0")
    iso_tools.build_iso(WORK / "root.ext2", WORK / "reliefos-clang.iso", WORK / "grub.cfg", WORK)


def guest(timeout, memory_mib=8192):
    serial = WORK / "guest-serial.log"
    serial.write_text("")
    with tempfile.TemporaryDirectory(prefix="clang-qmp-") as directory, \
            (WORK / "qemu.log").open("w") as errors:
        qmp = Path(directory) / "qmp.sock"
        process = subprocess.Popen([
            "qemu-system-x86_64", "-enable-kvm", "-cpu", "host", "-machine", "q35",
            "-m", str(memory_mib),
            "-smp", "2,sockets=1,cores=2,threads=1", "-bios", "/usr/share/edk2/x64/OVMF.4m.fd",
            "-display", "none", "-serial", f"file:{serial}", "-device", "VGA,xres=1280,yres=720",
            "-netdev", "user,id=net0", "-device", "e1000,netdev=net0",
            "-cdrom", str(WORK / "reliefos-clang.iso"), "-boot", "d",
            "-qmp", f"unix:{qmp},server=on,wait=off", "-no-reboot", "-no-shutdown"],
            stdout=subprocess.DEVNULL, stderr=errors)
        deadline = time.monotonic() + timeout
        try:
            while process.poll() is None and time.monotonic() < deadline:
                text = serial.read_text(errors="replace")
                if "[clang-probe] DONE" in text or "KERNEL PANIC" in text:
                    break
                time.sleep(0.5)
        finally:
            if process.poll() is None:
                iso_tools.qmp_quit(qmp, process)
    text = serial.read_text(errors="replace")
    markers = ("[clang-probe]", "[reliefnt] mmap", "[reliefnt] ELF",
               "Error loading shared library",
               "Error relocating", "KERNEL PANIC")
    print("\n".join(line for line in text.splitlines() if any(m in line for m in markers)))
    if "[clang-probe] DONE failures=0" not in text:
        raise RuntimeError(f"Clang guest validation failed; see {serial}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--refresh", action="store_true")
    parser.add_argument("--run-only", action="store_true")
    parser.add_argument("--packages", nargs="+", default=PACKAGES,
                        help="Alpine packages to install into the guest root")
    parser.add_argument("--memory-mib", type=int, default=8192,
                        help="guest RAM; the loader must relocate the whole root module below 4 GiB")
    parser.add_argument("--timeout", type=int, default=900)
    args = parser.parse_args()
    WORK.mkdir(parents=True, exist_ok=True)
    layout()
    if not args.run_only:
        prepare(args.packages, args.refresh)
    guest(args.timeout, args.memory_mib)
