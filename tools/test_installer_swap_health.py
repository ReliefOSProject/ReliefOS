#!/usr/bin/env python3
"""Check only the configured optional /swapfile against actual proc state."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("--output",type=Path)
    args=parser.parse_args()
    if args.output:args.output.mkdir(parents=True,exist_ok=False)
    else:args.output=Path(tempfile.mkdtemp(prefix="installer-swap-health-",dir=os.environ.get("TMPDIR")))
    script=ROOT/"system/rootfs/usr/lib/reliefos/check-swapfile"
    if not script.is_file():
        print("FAIL: installed optional swap health check is absent",flush=True);return 1
    failures=0
    for name,fstab,swaps,expected in (
        ("disabled","/dev/root / ext4 defaults 0 1\n","Filename Type Size Used Priority\n",0),
        ("unrelated","UUID=old none swap sw 0 0\n","Filename Type Size Used Priority\n",0),
        ("noauto","/swapfile none swap noauto 0 0\n","Filename Type Size Used Priority\n",0),
        ("active","/swapfile none swap sw 0 0\n","Filename Type Size Used Priority\n/swapfile file 524284 0 -2\n",0),
        ("missing","/swapfile none swap sw 0 0\n","Filename Type Size Used Priority\n",1),
    ):
        case=args.output/name;case.mkdir()
        (case/"fstab").write_text(fstab);(case/"swaps").write_text(swaps)
        command=["sh",str(script),str(case/"fstab"),str(case/"swaps")]
        print("+ "+shlex.join(command),flush=True)
        result=subprocess.run(command,cwd=ROOT,check=False)
        print("exit="+str(result.returncode)+" expected="+str(expected),flush=True)
        failures+=result.returncode!=expected
    return 1 if failures else 0


if __name__=="__main__":raise SystemExit(main())
