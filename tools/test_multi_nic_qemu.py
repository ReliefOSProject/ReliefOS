#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise three independent NICs using an owned minimal ReliefOS QEMU guest."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

from test_e1000e_qemu import echo_peers

ROOT = Path(__file__).resolve().parents[1]


def guest_script(port, modular):
    load = "modprobe e1000\nmodprobe e1000e\n" if modular else ""
    lifecycle = f'''
rm -f /run/multinic-hold-ready
/probe eth2 10.0.4.2 fec2::2 {port} hold &
holder=$!
for attempt in $(seq 1 100); do
 test ! -f /run/multinic-hold-ready || break
 sleep 0.1
done
test -f /run/multinic-hold-ready
rmmod e1000
if ifconfig eth0 >/dev/null 2>&1; then exit 1; fi
if ifconfig eth1 >/dev/null 2>&1; then exit 1; fi
ping -c 1 -W 3 -I eth2 10.0.4.2
wait "$holder"
echo MULTINIC_OTHER_OWNER_SURVIVED
modprobe e1000
configure eth3 10.0.2.2 fec0::15 fec0::2
configure eth4 10.0.3.2 fec1::15 fec1::2
/probe eth3 10.0.2.2 fec0::2 {port}
/probe eth4 10.0.3.2 fec1::2 {port}
echo MULTINIC_RELOAD_PASS
''' if modular else ""
    return f'''#!/bin/sh
export PATH=/bin
exec </dev/console >/dev/ttyS0 2>&1
finish() {{
 status=$?
 echo MULTINIC_QEMU_DONE:status=$status
 sync
 poweroff -f
 while :; do sleep 1; done
}}
trap finish EXIT
set -eu
test -c /dev/console
test -r /proc/net/dev
echo MULTINIC_QEMU_START
uname -a
{load}
configure() {{
 interface=$1
 udhcpc -f -n -q -i "$interface" -s /dhcp-script -t 4 -T 1
 ping -c 1 -W 3 -I "$interface" "$2"
 ip -6 addr add "$3/64" dev "$interface"
 attempts=0
 until ping6 -c 1 -W 1 -I "$interface" "$4"; do
  attempts=$((attempts + 1))
  test "$attempts" -lt 6
  sleep 1
 done
 echo MULTINIC_CONFIGURED:$interface
}}
configure eth0 10.0.2.2 fec0::15 fec0::2
configure eth1 10.0.3.2 fec1::15 fec1::2
configure eth2 10.0.4.2 fec2::15 fec2::2
/usr/lib/reliefos/network-profile check eth1
test "$(/usr/lib/reliefos/network-profile mode eth1)" = static
/usr/lib/reliefos/network-profile prepare eth1
grep -q '^nameserver 10.0.3.3$' /etc/resolv.conf
ping -c 1 -W 3 -I eth0 10.0.2.2
ping -c 1 -W 3 -I eth2 10.0.4.2
echo MULTINIC_OS_PROFILE_PASS
cat /proc/net/dev /proc/net/route /proc/net/if_inet6
/probe eth0 10.0.2.2 fec0::2 {port} &
first=$!
/probe eth1 10.0.3.2 fec1::2 {port} &
second=$!
/probe eth2 10.0.4.2 fec2::2 {port} &
third=$!
wait "$first"
wait "$second"
wait "$third"
echo MULTINIC_CONCURRENT_PASS
{lifecycle}
cat /proc/net/dev
awk '$1 ~ /^eth[0-9]+:/ && $4 != 0 {{exit 1}}' /proc/net/dev
ip addr show
ip route show
echo MULTINIC_QEMU_PASS
'''


def prepare(args, port):
    work = args.output
    root = work / "root"
    for name in ("bin", "dev", "proc", "sys", "tmp", "run", "etc"):
        (root / name).mkdir(parents=True, exist_ok=True)
    shutil.copy2(args.busybox, root / "bin/busybox")
    for name in subprocess.check_output([str(args.busybox), "--list"], text=True).splitlines():
        (root / "bin" / name).symlink_to("busybox")
    (root / "sbin").symlink_to("bin")
    (root / "usr").mkdir()
    (root / "usr/bin").symlink_to("../bin")
    (root / "usr/sbin").symlink_to("../bin")
    profile_dir = root / "etc/reliefos/network.d"
    profile_dir.mkdir(parents=True)
    (profile_dir / "eth1.conf").write_text("version=1\nmode=static\naddress=10.0.3.15\nprefix=24\n"
                                          "gateway=0.0.0.0\ndns_mode=custom\ndns_servers=10.0.3.3\n")
    helper_dir = root / "usr/lib/reliefos"
    helper_dir.mkdir(parents=True)
    for name in ("network-profile", "publish-resolver"):
        shutil.copy2(ROOT / "system/rootfs/usr/lib/reliefos" / name, helper_dir / name)
    shutil.copy2(args.probe, root / "probe")
    manifest = json.loads((args.install / "install.bundle.json").read_text())
    module_path = Path(manifest["module_directory"])
    shutil.copytree(args.install / module_path, root / module_path)
    subprocess.run([str(args.busybox), "depmod", "-b", str(root), manifest["release"]], check=True)
    (root / "init").write_text(guest_script(port, args.modular))
    (root / "init").chmod(0o755)
    hook = root / "dhcp-script"
    hook.write_text('''#!/bin/sh
set -eu
case "$1" in
 bound|renew)
  ifconfig "$interface" "$ip" netmask "$subnet" up
  echo "MULTINIC_DHCP:$interface:$ip"
  ;;
esac
''')
    hook.chmod(0o755)
    archive = work / "initramfs.cpio"
    names = ["."] + sorted(str(path.relative_to(root)) for path in root.rglob("*"))
    with archive.open("wb") as output:
        subprocess.run(["cpio", "--null", "-o", "--format=newc", "--owner=0:0"],
                       input=("\0".join(names) + "\0").encode(), cwd=root, stdout=output, check=True)
    iso_root = work / "iso"
    boot = iso_root / "boot"
    (boot / "grub").mkdir(parents=True)
    kernel = args.install / "kernel.sys"
    wanted = manifest["files"]["kernel.sys"]["sha256"]
    if hashlib.sha256(kernel.read_bytes()).hexdigest() != wanted:
        raise ValueError("installed kernel differs from its manifest")
    shutil.copy2(kernel, boot / "kernel.sys")
    shutil.copy2(archive, boot / "initramfs.cpio")
    shutil.copy2("/usr/share/grub/unicode.pf2", boot / "grub/unicode.pf2")
    (boot / "grub/grub.cfg").write_text('''set timeout=0
serial --unit=0 --speed=115200
terminal_input serial
terminal_output serial
insmod all_video
insmod gfxterm
loadfont /boot/grub/unicode.pf2
set gfxmode=1024x768x32
set gfxpayload=keep
terminal_output gfxterm serial
menuentry "ReliefOS multiple NIC test" {
 multiboot2 /boot/kernel.sys root=/ log=serial,screen rdinit=/init
 module2 /boot/initramfs.cpio initramfs
 boot
}
''')
    iso = work / "qa.iso"
    with (work / "iso.log").open("w") as output:
        subprocess.run(["grub-mkrescue", "-o", str(iso), str(iso_root)],
                       stdout=output, stderr=output, check=True)
    return iso, manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("install", "busybox", "probe", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--modular", action="store_true")
    parser.add_argument("--timeout", type=float, default=180)
    args = parser.parse_args()
    for name in ("install", "busybox", "probe", "output"):
        setattr(args, name, getattr(args, name).resolve())
    if args.output.exists() or not args.output.is_relative_to(ROOT / "out"):
        parser.error("output must be a new directory inside this checkout's out directory")
    args.output.mkdir(parents=True)
    result = {"passed": False, "vmware_verified": False, "physical_hardware_verified": False}
    with echo_peers() as (port, counts):
        iso, manifest = prepare(args, port)
        serial = args.output / "serial.log"
        command = ["qemu-system-x86_64", "-machine", "q35", "-accel", "kvm", "-cpu", "host",
                   "-m", "1024", "-smp", "2", "-vga", "std", "-display", "none",
                   "-serial", "file:" + str(serial), "-cdrom", str(iso), "-boot", "d",
                   "-nic", "none", "-no-reboot"]
        for index, model in enumerate(("e1000", "e1000", "e1000e")):
            network = (f"user,id=net{index},net=10.0.{index+2}.0/24,"
                       f"dhcpstart=10.0.{index+2}.15,ipv6-prefix=fec{index}::")
            command += ["-netdev", network, "-device", f"{model},netdev=net{index}"]
        result.update(command=command, release=manifest["release"],
                      kernel_sha256=manifest["files"]["kernel.sys"]["sha256"],
                      probe_sha256=hashlib.sha256(args.probe.read_bytes()).hexdigest(),
                      busybox_sha256=hashlib.sha256(args.busybox.read_bytes()).hexdigest())
        with (args.output / "qemu.log").open("w") as log:
            process = subprocess.Popen(command, stdout=log, stderr=log,
                                       env={**os.environ, "TMPDIR": str(ROOT / "out/tmp")})
            try:
                deadline = time.monotonic() + args.timeout
                while process.poll() is None and time.monotonic() < deadline:
                    time.sleep(.25)
                if process.poll() is None:
                    raise TimeoutError("multiple NIC guest timed out")
                text = serial.read_text(errors="replace") if serial.exists() else ""
                for marker in ("MULTINIC_CONCURRENT_PASS", "MULTINIC_OS_PROFILE_PASS", "MULTINIC_QEMU_PASS", "MULTINIC_QEMU_DONE:status=0"):
                    if marker not in text:
                        raise AssertionError("missing guest result " + marker)
                if "MULTINIC_FAIL" in text or "Kernel Panic" in text or "SIOCGIFTXQLEN:" in text or process.returncode:
                    raise AssertionError("guest failed")
                if args.modular:
                    for marker in ("MULTINIC_OTHER_OWNER_SURVIVED", "MULTINIC_RELOAD_PASS"):
                        if marker not in text:
                            raise AssertionError("missing module lifecycle result " + marker)
                result.update(passed=True, returncode=process.returncode, echoes=dict(counts))
            except Exception as error:
                result["error"] = repr(error)
                raise
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
