#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise installed storage drivers on two new disposable ext4 QEMU disks.

Only paths inside a newly created out directory are written. Native syscall
and data-integrity coverage lives in the ReliefNT-owned guest probe.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

ROOT=Path(__file__).resolve().parents[1]


def guest_script(modular):
    load="modprobe ahci\nmodprobe nvme\n" if modular else ""
    reload="""modprobe -r nvme
modprobe -r ahci
modprobe ahci
modprobe nvme
mount -t ext4 /dev/nvme0n1 /mnt/nvme
mount -t ext4 /dev/sda /mnt/ahci
sha256sum -c /checksums
umount /mnt/nvme
umount /mnt/ahci
echo STORAGE_RELOAD_PASS
""" if modular else ""
    return """#!/bin/sh
export PATH=/bin
exec </dev/console >/dev/ttyS0 2>&1
finish() {
 status=$?
 echo STORAGE_QEMU_DONE:status=$status
 poweroff -f
 while :; do sleep 1; done
}
trap finish EXIT
set -eu
"""+load+"""
cat /proc/partitions
mkdir -p /mnt/nvme /mnt/ahci
mount -t ext4 /dev/nvme0n1 /mnt/nvme
mount -t ext4 /dev/sda /mnt/ahci
/probe /mnt/nvme/payload &
nvme_pid=$!
/probe /mnt/ahci/payload &
ahci_pid=$!
wait "$nvme_pid"
wait "$ahci_pid"
sha256sum /mnt/nvme/payload /mnt/ahci/payload > /checksums
umount /mnt/nvme
umount /mnt/ahci
"""+reload+"""
cat /proc/interrupts /proc/diskstats
echo STORAGE_QEMU_PASS
"""


def prepare(args):
    root=args.output/"root"
    for name in ("bin","dev","proc","sys","tmp","run","etc"):
        (root/name).mkdir(parents=True,exist_ok=True)
    shutil.copy2(args.busybox,root/"bin/busybox")
    for name in subprocess.check_output([str(args.busybox),"--list"],text=True).splitlines():
        (root/"bin"/name).symlink_to("busybox")
    (root/"sbin").symlink_to("bin")
    shutil.copy2(args.probe,root/"probe")
    manifest=json.loads((args.install/"install.bundle.json").read_text())
    wanted="m" if args.modular else "y"
    for option in ("BLK_DEV_NVME","SATA_AHCI"):
        if manifest["configuration"].get(option)!=wanted:
            raise ValueError(f"expected CONFIG_{option}={wanted}")
    kernel=args.install/"kernel.sys"
    if hashlib.sha256(kernel.read_bytes()).hexdigest()!=manifest["files"]["kernel.sys"]["sha256"]:
        raise ValueError("installed kernel manifest mismatch")
    modules=Path(manifest["module_directory"])
    shutil.copytree(args.install/modules,root/modules)
    subprocess.run([str(args.busybox),"depmod","-b",str(root),manifest["release"]],check=True)
    (root/"init").write_text(guest_script(args.modular));(root/"init").chmod(0o755)
    names=["."]+sorted(str(p.relative_to(root)) for p in root.rglob("*"))
    boot=args.output/"iso/boot";(boot/"grub").mkdir(parents=True)
    with (boot/"initramfs.cpio").open("wb") as stream:
        subprocess.run(["cpio","--null","-o","--format=newc","--owner=0:0"],
                       input=("\0".join(names)+"\0").encode(),cwd=root,stdout=stream,check=True)
    shutil.copy2(kernel,boot/"kernel.sys")
    shutil.copy2("/usr/share/grub/unicode.pf2",boot/"grub/unicode.pf2")
    (boot/"grub/grub.cfg").write_text("""set timeout=0
serial --unit=0 --speed=115200
terminal_output serial
insmod all_video
insmod gfxterm
loadfont /boot/grub/unicode.pf2
set gfxmode=1024x768x32
set gfxpayload=keep
menuentry "ReliefOS storage IRQ test" {
 multiboot2 /boot/kernel.sys root=/ log=serial,screen rdinit=/init
 module2 /boot/initramfs.cpio initramfs
 boot
}
""")
    iso=args.output/"qa.iso"
    with (args.output/"iso.log").open("w") as log:
        subprocess.run(["grub-mkrescue","-o",str(iso),str(boot.parent)],stdout=log,stderr=log,check=True)
    for transport in ("nvme","ahci"):
        disk=args.output/(transport+".raw")
        with disk.open("xb") as stream:stream.truncate(64*1024*1024)
        subprocess.run(["mkfs.ext4","-q","-F","-b","4096","-O","^64bit,^metadata_csum,^orphan_file",str(disk)],check=True)
    return iso,manifest


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("install","busybox","probe","output"):
        parser.add_argument("--"+name,type=Path,required=True)
    parser.add_argument("--modular",action="store_true")
    parser.add_argument("--cpus",type=int,choices=(1,4),default=4)
    parser.add_argument("--timeout",type=float,default=180)
    args=parser.parse_args()
    for name in ("install","busybox","probe","output"):
        setattr(args,name,getattr(args,name).resolve())
    if args.output.exists() or not args.output.is_relative_to(ROOT/"out"):
        parser.error("output must be a new directory inside out")
    args.output.mkdir(parents=True)
    result={"passed":False,"vmware_verified":False,"physical_hardware_verified":False}
    iso,manifest=prepare(args)
    serial=args.output/"serial.log"
    command=["qemu-system-x86_64","-machine","q35","-accel","kvm","-cpu","host","-m","1024",
             "-smp",str(args.cpus),"-display","none","-serial","file:"+str(serial),"-nic","none",
             "-cdrom",str(iso),"-boot","d","-no-reboot"]
    for transport in ("nvme","ahci"):
        command += ["-drive",f"file={args.output/(transport+'.raw')},format=raw,if=none,id={transport},throttling.iops-read=200"]
    command += ["-device","nvme,drive=nvme,serial=reliefos-storage-irq","-device","ide-hd,drive=ahci,bus=ide.1"]
    result.update(command=command,release=manifest["release"],kernel_sha256=manifest["files"]["kernel.sys"]["sha256"])
    with (args.output/"qemu.log").open("w") as log:
        process=subprocess.Popen(command,stdout=log,stderr=log,env={**os.environ,"TMPDIR":str(ROOT/"out/tmp")})
        try:
            deadline=time.monotonic()+args.timeout
            while process.poll() is None and time.monotonic()<deadline:time.sleep(.25)
            if process.poll() is None:raise TimeoutError("storage guest timed out")
            output=serial.read_text(errors="replace") if serial.exists() else ""
            markers=["STORAGE_GUEST_PASS path=/mnt/nvme/payload","STORAGE_GUEST_PASS path=/mnt/ahci/payload",
                     "STORAGE_QEMU_PASS","STORAGE_QEMU_DONE:status=0","irq=msix","irq=msi"]
            if args.modular:markers.append("STORAGE_RELOAD_PASS")
            for marker in markers:
                if marker not in output:raise AssertionError("missing guest result "+marker)
            rows=re.findall(r"(?m)^8[0-9]:\s+([0-9 ]+)\s+.*PCI-MSI",output)
            counts=[sum(map(int,row.split())) for row in rows]
            if len([n for n in counts if n])<2:raise AssertionError("missing real storage MSI/MSI-X interrupts")
            if process.returncode or "STORAGE_FAIL" in output or "Kernel Panic" in output:
                raise AssertionError("storage guest failed")
            expected=hashlib.sha256(bytes(((i*17)^(i>>9)^(i>>17)^0xa7)&255 for i in range(4*32*65536))).hexdigest()
            for transport in ("nvme","ahci"):
                extracted=args.output/(transport+"-payload")
                subprocess.run(["debugfs","-R",f"dump /payload {extracted}",str(args.output/(transport+".raw"))],check=True,capture_output=True)
                if hashlib.sha256(extracted.read_bytes()).hexdigest()!=expected:raise AssertionError("host data mismatch "+transport)
            result.update(passed=True,returncode=process.returncode,interrupt_counts=counts,payload_sha256=expected)
        except Exception as error:
            result["error"]=repr(error);raise
        finally:
            if process.poll() is None:
                process.terminate()
                try:process.wait(timeout=5)
                except subprocess.TimeoutExpired:process.kill();process.wait()
            (args.output/"result.json").write_text(json.dumps(result,indent=2)+"\n")
    print(json.dumps(result,indent=2))


if __name__=="__main__":main()
