#!/usr/bin/env python3
"""Run upstream apk and signed package transactions on the ReliefNT kernel."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

from apk_distribution import ROOT, bootstrap, make_package, run, signing_key
from make_live_root import make_live_tree
from make_ext2_root import write_ext2_root
import test_linux_ioctl_cloexec as iso_tools

WORK = ROOT / "build/apk-qemu"


def prepare(package_cache=None, guest_proxy=None, testing_only=False):
    apk, key = bootstrap(), signing_key()
    managed = ROOT / "build/apk/root"
    if not (managed / "lib/apk/db/installed").is_file():
        raise RuntimeError("run python3 build.py run apk-root first")
    sysroot = ROOT / "build/musl/sysroot"
    run(["clang", "--target=x86_64-linux-musl", "-fuse-ld=lld", "-nostdlib", "-nostdinc",
         "-isystem", sysroot / "include", "-static", "-O2", "-Wall", "-Wextra",
         *(["-DAPK_TESTING_ONLY"] if testing_only else []),
         sysroot / "lib/crt1.o", sysroot / "lib/crti.o", ROOT / "tools/tests/apk_guest_probe.c",
         ROOT / "kernel/reliefnt/tools/tests/linux_map_stack_test.c",
         sysroot / "lib/libc.a", sysroot / "lib/crtn.o", "-o", WORK / "probe.elf"])
    run(["clang", "--target=x86_64-linux-musl", "-fuse-ld=lld", "-nostdlib", "-nostdinc",
         "-isystem", sysroot / "include", "-O2", "-Wall", "-Wextra",
         sysroot / "lib/crt1.o", sysroot / "lib/crti.o", ROOT / "tools/tests/apk_zlib_probe.c",
         "-L" + str(sysroot / "lib"), "-lc", sysroot / "lib/crtn.o",
         "-Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1", "-o", WORK / "zlib-probe.elf"])
    headers = run(["readelf", "-l", WORK / "zlib-probe.elf"], capture_output=True, text=True).stdout
    if "[Requesting program interpreter: /lib/ld-musl-x86_64.so.1]" not in headers:
        raise RuntimeError("zlib probe must be a genuinely dynamic musl executable")
    run([apk, "--root", managed, "--repositories-file", ROOT / "system/rootfs/etc/apk/repositories",
         "update"])
    upstream = WORK / "upstream"
    upstream.mkdir(exist_ok=True)
    run([apk, "--root", managed, "--repositories-file", ROOT / "system/rootfs/etc/apk/repositories",
         "fetch", "--output", upstream, "zlib"])
    with tempfile.TemporaryDirectory(prefix="stage-", dir=WORK) as directory:
        stage = Path(directory) / "root"
        make_live_tree(managed, stage)
        shutil.rmtree(stage / "var/cache/apk")
        (stage / "var/cache/apk").mkdir(mode=0o755)
        tests = stage / "usr/lib/reliefos/tests"
        tests.mkdir(parents=True, exist_ok=True)
        shutil.copy2(WORK / "probe.elf", tests / "linux-inventory.elf")
        shutil.copy2(WORK / "zlib-probe.elf", tests / "apk-zlib.elf")
        fixtures = tests / "apk"
        fixtures.mkdir()
        if guest_proxy is not None:
            if not guest_proxy or len(guest_proxy.encode()) >= 1024 or any(c.isspace() for c in guest_proxy):
                raise ValueError("guest proxy must be a nonempty URL without whitespace, shorter than 1024 bytes")
            (fixtures / "proxy").write_text(guest_proxy)
        for version, label, default in (("1.0-r0", "one", "original"), ("1.1-r0", "two", "new default")):
            repo = fixtures / ("v1" if label == "one" else "v2")
            repo.mkdir()
            payload = Path(directory) / label
            (payload / "usr/share/apk-probe").mkdir(parents=True)
            (payload / "etc").mkdir()
            (payload / "usr/share/apk-probe/value").write_text(f"version {label}\n")
            (payload / "etc/apk-probe.conf").write_text(default + "\n")
            scripts = []
            for kind, marker in (("post-install", "post-install"), ("trigger", "trigger")):
                script = Path(directory) / f"{label}-{kind}.sh"
                script.write_text(f"#!/bin/sh\necho {marker} > /tmp/apk-{marker}\n")
                scripts.append((kind, script))
            package = make_package(apk, key, payload, repo / f"reliefos-apk-probe-{version}.apk",
                                   "reliefos-apk-probe", version, ["reliefos-busybox"],
                                   scripts=scripts, triggers=["/usr/share/apk-probe"])
            run([apk, "mkndx", "--keys-dir", stage / "etc/apk/keys", "--sign-key", key,
                 "--output", repo / "packages.adb", package])
        repo = fixtures / "conflict"
        repo.mkdir()
        package = make_package(apk, key, Path(directory) / "empty", repo / "fastfetch-1.0-r0.apk",
                               "fastfetch", "1.0-r0", [])
        run([apk, "mkndx", "--keys-dir", stage / "etc/apk/keys", "--sign-key", key,
             "--output", repo / "packages.adb", package])
        make_package(apk, None, Path(directory) / "empty", fixtures / "unsigned.apk",
                     "unsigned-test", "1.0-r0", [])
        zlib_packages = list(upstream.glob("zlib-*.apk"))
        if len(zlib_packages) != 1:
            raise RuntimeError("ambiguous Alpine zlib fixture; clean build/apk-qemu/upstream")
        shutil.copyfile(zlib_packages[0], fixtures / "zlib.apk")
        if package_cache is not None:
            cache = tests / "apk-cache"
            cache.mkdir()
            for package in package_cache.glob("*.apk"):
                shutil.copyfile(package, cache / package.name)
        # The cache fixture and writable cache coexist with GCC's extracted files.
        write_ext2_root(stage, WORK / "root.ext2", minimum_mib=768 if package_cache is not None else 512)
    iso_tools.GRUB_TEMPLATE = iso_tools.GRUB_TEMPLATE.replace(
        "autospawn=ioctlcloexec autospawn=python315", "autospawn=inventory").replace(
        "syscall-trace=/opt/python/", "").replace("set timeout=5", "set timeout=0")
    iso_tools.build_iso(WORK / "root.ext2", WORK / "reliefos-apk.iso", WORK / "grub.cfg", WORK)


def guest(timeout):
    serial = WORK / "guest-serial.log"
    serial.write_text("")
    with tempfile.TemporaryDirectory(prefix="apk-qmp-") as directory, (WORK / "qemu.log").open("w") as errors:
        qmp = Path(directory) / "qmp.sock"
        process = subprocess.Popen([
            "qemu-system-x86_64", "-enable-kvm", "-cpu", "host", "-machine", "q35", "-m", "4096",
            "-smp", "2,sockets=1,cores=2,threads=1", "-bios", "/usr/share/edk2/x64/OVMF.4m.fd",
            "-display", "none", "-serial", f"file:{serial}", "-device", "VGA,xres=1280,yres=720",
            "-netdev", "user,id=net0", "-device", "e1000,netdev=net0", "-cdrom", str(WORK / "reliefos-apk.iso"),
            "-boot", "d", "-qmp", f"unix:{qmp},server=on,wait=off", "-no-reboot", "-no-shutdown"],
            stdout=subprocess.DEVNULL, stderr=errors)
        deadline = time.monotonic() + timeout
        try:
            while process.poll() is None and time.monotonic() < deadline:
                text = serial.read_text(errors="replace")
                if "[apk-probe] DONE" in text or "KERNEL PANIC" in text:
                    break
                time.sleep(0.5)
        finally:
            if process.poll() is None:
                iso_tools.qmp_quit(qmp, process)
    text = serial.read_text(errors="replace")
    print("\n".join(line for line in text.splitlines() if "[apk-probe]" in line))
    if "[apk-probe] DONE failures=0" not in text:
        raise RuntimeError(f"APK guest validation failed; see {serial}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-only", action="store_true")
    parser.add_argument("--testing-only", action="store_true",
                        help="install and run upstream HyFetch from the tagged testing repository")
    parser.add_argument("--guest-proxy", help="optional proxy URL reachable from QEMU; default is direct access")
    parser.add_argument("--package-cache", type=Path,
                        help="seed the GCC transaction from real APK cache files; earlier HTTPS tests stay online")
    parser.add_argument("--timeout", type=int, default=420)
    args = parser.parse_args()
    WORK.mkdir(parents=True, exist_ok=True)
    if not args.run_only:
        prepare(args.package_cache, args.guest_proxy, args.testing_only)
    guest(args.timeout)
