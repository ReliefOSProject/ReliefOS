#!/usr/bin/env python3
"""Exercise the shared TTY account, loader and optional swap controls."""
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


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("--output",type=Path)
    args=parser.parse_args()
    if args.output:args.output.mkdir(parents=True,exist_ok=False)
    else:args.output=Path(tempfile.mkdtemp(prefix="installer-swap-tty-",dir=os.environ.get("TMPDIR")))
    cc=shutil.which("clang")
    if not cc:raise RuntimeError("clang is required")
    binary=str(args.output.resolve()/"tty")
    command=[cc,"-std=gnu11","-D_GNU_SOURCE","-O1","-g","-Wall","-Wextra","-Werror",
             "-fsanitize=address,undefined","-fno-sanitize-recover=all","-fno-pie","-no-pie",
             "-ffunction-sections","-fdata-sections","-Wl,--gc-sections,--wrap=read,--wrap=write,--wrap=tcgetattr,--wrap=tcsetattr",
             "-Ikernel/reliefnt/include","-Iinclude","-Ikernel/reliefnt/include/uapi","-idirafter","userland/runtime/include",
             "tools/tests/installer_tty_input_test.c","userland/apps/installer/installer_capacity.c","-o",binary]
    return run(command) or run([binary])


if __name__=="__main__":raise SystemExit(main())
