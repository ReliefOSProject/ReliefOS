#!/usr/bin/env python3
"""Run the shared installer swap model/capacity regression."""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]


def run(args):
    print("+ " + shlex.join(args), flush=True)
    result=subprocess.run(args,cwd=ROOT,check=False)
    print("exit="+str(result.returncode),flush=True)
    return result.returncode


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output",type=Path)
    args=parser.parse_args()
    if args.output:
        args.output.mkdir(parents=True,exist_ok=False)
    else:
        args.output=Path(tempfile.mkdtemp(prefix="installer-swap-",dir=os.environ.get("TMPDIR")))
    cc=shutil.which("clang")
    if not cc:
        raise RuntimeError("clang is required")
    binary=str(args.output.resolve()/"model")
    command=[cc,"-std=c11","-D_GNU_SOURCE","-O1","-g","-Wall","-Wextra","-Werror",
             "-fsanitize=address,undefined","-fno-sanitize-recover=all","-fno-pie","-no-pie",
             "-ffunction-sections","-fdata-sections","-Wl,--gc-sections",
             "tools/tests/installer_swap_model_test.c","userland/apps/installer/installer_capacity.c",
             "-o",binary]
    return run(command) or run([binary])


if __name__=="__main__":
    raise SystemExit(main())
