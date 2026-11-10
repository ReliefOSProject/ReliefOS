#!/usr/bin/env python3
"""Verify swap capability from complete checksum-bound target bundles."""
from runtime_test_dependencies import crypto_flags
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
RELEASE="5.1.2-ko1-aaaaaaaaaaaa"


def run(args):
    print("+ "+shlex.join(args),flush=True)
    result=subprocess.run(args,cwd=ROOT,check=False)
    print("exit="+str(result.returncode),flush=True)
    return result.returncode


def make_bundle(root,config,corrupt=False):
    boot="usr/lib/reliefos/kernel/"+RELEASE+"/"
    modules="lib/modules/"+RELEASE+"/"
    kernel=b"kernel fixture\n";initramfs=b"initramfs fixture\n";abi="a"*64
    environment=("# GRUB Environment Block\nreliefos_generation=1\nreliefos_release="+RELEASE+
        "\nreliefos_module_abi="+abi+"\nreliefos_kernel_sha256="+hashlib.sha256(kernel).hexdigest()+
        "\nreliefos_initramfs_sha256="+hashlib.sha256(initramfs).hexdigest()+"\n").encode()
    files={boot+"kernel.sys":kernel,boot+"kerneldebug.sys":b"debug\n",boot+"initramfs.cpio":initramfs,
           boot+"boot.env":environment+b"#"*(1024-len(environment)),modules+"config":config,
           modules+"module.abi":(abi+"\n").encode()}
    for name in ("Module.symvers","System.map","modules.order","modules.builtin","modules.builtin.modinfo",
                 "modules.dep","modules.alias","modules.symbols","modules-load.d/reliefnt-platform.conf"):
        files[modules+name]=b""
    for name,data in files.items():
        path=root/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data)
    sums="".join(hashlib.sha256(data).hexdigest()+"  "+name+"\n" for name,data in sorted(files.items()))
    (root/boot/"SHA256SUMS").write_text(sums)
    (root/"usr/lib/reliefos/kernel/current-release").write_text(RELEASE+"\n")
    if corrupt:(root/modules/"config").write_bytes(b"CONFIG_SWAP=y\n")


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("--output",type=Path)
    args=parser.parse_args()
    if args.output:args.output.mkdir(parents=True,exist_ok=False)
    else:args.output=Path(tempfile.mkdtemp(prefix="installer-bundle-swap-",dir=os.environ.get("TMPDIR")))
    args.output=args.output.resolve()
    cc=shutil.which("clang")
    if not cc:raise RuntimeError("clang is required")
    binary=str(args.output/"bundle")
    sources=["tools/tests/installer_bundle_swap_test.c","userland/runtime/src/kernel_bundle_check.c",
             "userland/runtime/src/kernel_bundle_io.c","userland/runtime/src/bootloader_publish.c",
             "userland/runtime/shared/boot_slots.c"]
    if (ROOT/"userland/apps/installer/installer_swap_bundle.c").exists():
        sources.append("userland/apps/installer/installer_swap_bundle.c")
    command=[cc,"-std=c11","-D_GNU_SOURCE","-O1","-g","-Wall","-Wextra","-Werror",
             "-fsanitize=address,undefined","-fno-sanitize-recover=all","-fno-pie","-no-pie",
             "-ffunction-sections","-fdata-sections","-Wl,--gc-sections",
             "-I",str(args.output),"-Iuserland/runtime/include",
             *sources,*crypto_flags(),"-o",binary]
    result=run(command)
    if result:return result
    failures=0
    for name,config,expected,corrupt in (("enabled",b"CONFIG_SWAP=y\n","yes",False),
       ("disabled",b"# CONFIG_SWAP is not set\n","no",False),
       ("duplicate",b"CONFIG_SWAP=y\n# CONFIG_SWAP is not set\n","error",False),
       ("missing",b"CONFIG_OTHER=y\n","error",False),
       ("corrupt",b"# CONFIG_SWAP is not set\n","error",True)):
        directory=args.output/name;make_bundle(directory,config,corrupt)
        failures+=bool(run([binary,str(directory),expected,name]))
    return 1 if failures else 0


if __name__=="__main__":raise SystemExit(main())
