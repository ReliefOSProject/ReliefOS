#!/usr/bin/env python3
"""Exercise fully allocated installer swapfile creation and failure rollback."""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]


def run(args, expected_stderr=None):
    print("+ "+shlex.join(args),flush=True)
    result=subprocess.run(args,cwd=ROOT,check=False,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    print(result.stdout,end='',flush=True)
    print(result.stderr,end='',file=__import__('sys').stderr,flush=True)
    print("exit="+str(result.returncode),flush=True)
    if expected_stderr and expected_stderr not in result.stderr:
        print('FAIL: missing secondary close diagnostic '+expected_stderr,flush=True)
        return 1
    return result.returncode


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("--output",type=Path)
    args=parser.parse_args()
    if args.output: args.output.mkdir(parents=True,exist_ok=False)
    else: args.output=Path(tempfile.mkdtemp(prefix="installer-swap-create-",dir=os.environ.get("TMPDIR")))
    cc=shutil.which("clang")
    if not cc: raise RuntimeError("clang is required")
    binary=str(args.output.resolve()/"create")
    sources=["tools/tests/installer_swap_create_test.c"]
    if (ROOT/"userland/apps/installer/installer_swap.c").exists():
        sources += ["userland/apps/installer/installer_swap.c","userland/apps/installer/installer_capacity.c"]
    command=[cc,"-std=c11","-D_GNU_SOURCE","-O1","-g","-Wall","-Wextra","-Werror","-fsanitize=address,undefined",
             "-fno-sanitize-recover=all","-fno-pie","-no-pie","-ffunction-sections","-fdata-sections",
             "-Wl,--gc-sections,--wrap=open,--wrap=openat,--wrap=close,--wrap=write,--wrap=fsync,--wrap=fchown,--wrap=fstat,--wrap=linkat,--wrap=unlinkat,--wrap=posix_spawnp,--wrap=waitpid",
             *sources,"-o",binary]
    result=run(command)
    if result:return result
    failures=0
    for case in ("success","eintr-short","enospc","spawn","mkswap","sync-before","sync-after","sync-dir",
                 "publish","unlink-temp","close-file","close-root","cleanup-fail","existing",
                 "owned-remove","owned-replaced","write-close-file","write-close-root",
                 "unsafe-root-mode","unsafe-root-owner","replace-before-link","replace-after-link",
                 "replace-final-after-link","replace-final-after-unlink"):
        directory=args.output/case;directory.mkdir()
        expected={'write-close-file':'swap file close failed','write-close-root':'swap target directory close failed'}.get(case)
        failures+=bool(run([binary,case,str(directory.resolve())],expected))
    return 1 if failures else 0


if __name__=="__main__":raise SystemExit(main())
