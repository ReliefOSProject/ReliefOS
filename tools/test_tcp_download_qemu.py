#!/usr/bin/env python3
"""Measure unmodified apk HTTPS downloads through e1000, without Internet access.

Run in a private network namespace:
unshare --user --map-root-user --net python3 tools/test_tcp_download_qemu.py
"""
import argparse
import functools
import hashlib
import http.server
import json
import os
from pathlib import Path
import re
import shutil
import ssl
import subprocess
import tempfile
import threading
import time

from apk_distribution import ROOT, bootstrap, run, signing_key
from test_network_qemu import inject
import test_linux_ioctl_cloexec as iso_tools


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--label", default="current")
    parser.add_argument("--package", default="gcc")
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--install", action="store_true", help="download/install gcc, reliefos-musl-dev and make, then compile/run stdio")
    args = parser.parse_args()
    # Bind privileged HTTPS only in an isolated network namespace owned by us.
    import fcntl
    with open("/proc/self/ns/net", "rb") as namespace:
        owner = fcntl.ioctl(namespace.fileno(), 0xb701)
        try:
            if os.fstat(owner).st_ino != os.stat("/proc/self/ns/user").st_ino or os.geteuid() != 0:
                raise SystemExit("run under unshare --user --map-root-user --net")
        finally:
            os.close(owner)
    run(["ip", "link", "set", "lo", "up"])
    work = ROOT / "build/tcp-download" / args.label
    work.mkdir(parents=True, exist_ok=True)
    repo = work / "repository"
    repo.mkdir(exist_ok=True)
    packages = list((ROOT / "build/alpine-runtime/cache").glob(args.package + "-[0-9]*.apk"))
    if len(packages) != 1:
        raise RuntimeError(f"expected one cached upstream {args.package} APK, got {packages}")
    source = packages[0]
    package = repo / re.sub(r"\.[0-9a-f]{8}\.apk$", ".apk", source.name)
    shutil.copyfile(source, package)
    indexed = [package]
    if args.install:
        if args.package != "gcc":
            raise ValueError("--install requires --package gcc")
        for cached in sorted((ROOT / "build/alpine-runtime/cache").glob("*.apk")):
            target = repo / re.sub(r"\.[0-9a-f]{8}\.apk$", ".apk", cached.name)
            if target == package:
                continue
            shutil.copyfile(cached, target)
            indexed.append(target)
    apk, key = bootstrap(), signing_key()
    run([apk, "mkndx", "--keys-dir", ROOT / "build/apk/root/etc/apk/keys", "--sign-key", key,
         "--output", repo / "packages.adb", *indexed])
    certificate, private = work / "server.crt", work / "server.key"
    run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "30",
         "-subj", "/CN=10.0.2.2", "-addext", "subjectAltName=IP:10.0.2.2",
         "-out", certificate, "-keyout", private], capture_output=True)
    sysroot = ROOT / "build/musl/sysroot"
    probe = work / "download.elf"
    run(["clang", "--target=x86_64-linux-musl", "-fuse-ld=lld", "-nostdlib", "-nostdinc",
         "-isystem", sysroot / "include", "-static", "-O2", "-Wall", "-Wextra", "-Werror",
         sysroot / "lib/crt1.o", sysroot / "lib/crti.o", ROOT / "tools/tests/tcp_download_guest.c",
         sysroot / "lib/libc.a", sysroot / "lib/crtn.o", "-o", probe])
    image = work / "root.ext2"
    shutil.copyfile(ROOT / "build/live/root.ext2", image)
    run(["truncate", "-s", "768M" if args.install else "512M", image])
    run(["resize2fs", "-f", image], capture_output=True)
    run(["debugfs", "-w", "-R", "mkdir /usr/lib/reliefos/tests", image], capture_output=True)
    inject(image, probe, "/usr/lib/reliefos/tests/linux-inventory.elf", "0100755")
    repositories = work / "repositories"
    repositories.write_text("ndx /usr/share/reliefos/apk/repository/packages.adb\n"
                            "https://10.0.2.2/packages.adb\n")
    selection = work / "package"
    selection.write_text(args.package + "\n")
    inject(image, repositories, "/etc/apk/repositories")
    inject(image, certificate, "/etc/ssl/certs/ca-certificates.crt")
    inject(image, selection, "/tmp/download-package")
    if args.install:
        inject(image, selection, "/tmp/download-install")
    iso_tools.GRUB_TEMPLATE = iso_tools.GRUB_TEMPLATE.replace(
        "autospawn=ioctlcloexec autospawn=python315", "autospawn=inventory").replace(
        "syscall-trace=/opt/python/", "").replace("set timeout=5", "set timeout=0")
    iso = work / "download.iso"
    iso_tools.build_iso(image, iso, work / "grub.cfg", work)
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(repo))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 443), handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(certificate, private)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    serving = threading.Thread(target=server.serve_forever)
    serving.start()
    serial = work / "serial.log"
    serial.write_text("")
    try:
        with tempfile.TemporaryDirectory(prefix="tcp-qmp-") as temporary, (work / "qemu.log").open("w") as errors:
            qmp = Path(temporary) / "qmp"
            command = ["qemu-system-x86_64", "-enable-kvm", "-cpu", "host", "-machine", "q35",
                       "-m", "4096", "-smp", "2,sockets=1,cores=2,threads=1",
                       "-bios", "/usr/share/edk2/x64/OVMF.4m.fd", "-display", "none",
                       "-serial", f"file:{serial}", "-device", "VGA,xres=1280,yres=720",
                       "-netdev", "user,id=net0", "-device", "e1000-82545em,netdev=net0",
                       "-object", f"filter-dump,id=capture,netdev=net0,file={work / 'packets.pcap'}",
                       "-cdrom", str(iso), "-boot", "d", "-qmp", f"unix:{qmp},server=on,wait=off",
                       "-no-reboot", "-no-shutdown"]
            process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=errors)
            try:
                deadline = time.monotonic() + args.timeout
                while process.poll() is None and time.monotonic() < deadline:
                    text = serial.read_text(errors="replace") if serial.exists() else ""
                    if ("[tcp-download] DONE" in text and "name=sha256sum code=" in text) or "KERNEL PANIC" in text:
                        break
                    if re.search(r"\[tcp-download\] DONE status=[1-9]", text):
                        break
                    time.sleep(.25)
            finally:
                if process.poll() is None:
                    iso_tools.qmp_quit(qmp, process)
    finally:
        server.shutdown()
        server.server_close()
        serving.join()
    text = serial.read_text(errors="replace").replace("\\n", "\n")
    result = {"package": package.name, "bytes": package.stat().st_size,
              "install": args.install,
              "kernel_sha256": hashlib.sha256((work / "iso/leonos/kernel.sys").read_bytes()).hexdigest(),
              "complete": "[tcp-download] DONE status=0" in text, "serial_bytes": serial.stat().st_size}
    match = re.search(r"\[tcp-download\] DONE status=\d+ seconds=([0-9.]+)", text)
    if match:
        result["seconds"] = float(match[1])
        if not args.install:
            result["mib_per_second"] = result["bytes"] / 1048576 / result["seconds"]
    # Hash the actual file in the guest's writable live root.
    if result["complete"]:
        result["sha256"] = hashlib.sha256(source.read_bytes()).hexdigest()
        if args.install:
            assert "APK_GCC_NETWORK_OK" in text
            assert re.search(re.escape(result["sha256"]) + r"  /var/cache/apk/gcc-[^\s]+\.apk", text)
        else:
            assert result["sha256"] + "  /tmp/" + package.name in text
    (work / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2), flush=True)
    assert result["complete"], f"download failed or timed out: {serial}"


if __name__ == "__main__":
    main()
