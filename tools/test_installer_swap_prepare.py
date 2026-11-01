#!/usr/bin/env python3
"""Exercise fresh-install preparation sequencing and rollback ownership."""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path);a=p.parse_args()
    if a.output:a.output.mkdir(parents=True,exist_ok=False)
    else:a.output=Path(tempfile.mkdtemp(prefix='installer-prepare-',dir=os.environ.get('TMPDIR')))
    cc=shutil.which('clang');assert cc
    cmd=[cc,'-std=c11','-D_GNU_SOURCE','-O1','-g','-Wall','-Wextra','-Werror',
         '-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-pie','-no-pie',
         '-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
         'tools/tests/installer_swap_prepare_test.c','userland/apps/installer/installer_swap_prepare.c',
         'userland/apps/installer/installer_capacity.c','-o',str(a.output.resolve()/'test')]
    print('+ '+shlex.join(cmd),flush=True);ret=subprocess.run(cmd,cwd=ROOT,check=False).returncode
    print('exit='+str(ret),flush=True)
    if ret:return ret
    cmd=[str(a.output.resolve()/'test')];print('+ '+shlex.join(cmd),flush=True)
    ret=subprocess.run(cmd,cwd=ROOT,check=False).returncode;print('exit='+str(ret),flush=True);return ret
if __name__=='__main__':raise SystemExit(main())
