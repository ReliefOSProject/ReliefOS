#!/usr/bin/env python3
"""Test selected block identity through actual sysfs-style link topology."""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]


def run(args):
    print("+ "+shlex.join(args),flush=True)
    result=subprocess.run(args,cwd=ROOT,check=False)
    print("exit="+str(result.returncode),flush=True)
    return result.returncode


def fixture(root,kind):
    (root/"dev/block").mkdir(parents=True)
    (root/"bus/pci").mkdir(parents=True)
    if kind=="platform":
        disk=root/"devices/platform/leonos-block/sda"
    else:
        parent=root/"devices/pci0000:00/0000:00:03.0"
        disk=parent/"block/sda"
    disk.mkdir(parents=True)
    if kind!="platform":
        (parent/"subsystem").symlink_to("../../../bus/pci")
        if kind!="missing":
            (parent/"class").write_text("0x010802\n" if kind=="nvme" else "0x010601\n")
    target=os.path.relpath(disk,root/"dev/block")
    if kind=="escape":target="/etc"
    (root/"dev/block/8:0").symlink_to(target)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("--output",type=Path)
    args=parser.parse_args()
    if args.output:args.output.mkdir(parents=True,exist_ok=False)
    else:args.output=Path(tempfile.mkdtemp(prefix="installer-swap-storage-",dir=os.environ.get("TMPDIR")))
    args.output=args.output.resolve();cc=shutil.which("clang")
    if not cc:raise RuntimeError("clang is required")
    binary=str(args.output/"storage")
    sources=["tools/tests/installer_swap_storage_test.c"]
    if (ROOT/"userland/apps/installer/installer_swap_storage.c").exists():
        sources.append("userland/apps/installer/installer_swap_storage.c")
    command=[cc,"-std=c11","-D_GNU_SOURCE","-O1","-g","-Wall","-Wextra","-Werror",
             "-fsanitize=address,undefined","-fno-sanitize-recover=all","-fno-pie","-no-pie",
             "-ffunction-sections","-fdata-sections","-Wl,--gc-sections",*sources,"-o",binary]
    result=run(command)
    if result:return result
    failures=0
    for name in ("ahci","nvme","platform","missing","escape"):
        root=args.output/name/"sys";fixture(root,name)
        failures+=bool(run([binary,str(root),"yes" if name=="ahci" else "no",name]))
    return 1 if failures else 0


if __name__=="__main__":raise SystemExit(main())
