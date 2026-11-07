#!/usr/bin/env python3
"""Exercise fresh-target fstab/OpenRC publication and injected failures."""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[1]

def run(argv,expected_stderr=None):
    print('+ '+shlex.join(argv),flush=True)
    result=subprocess.run(argv,cwd=ROOT,check=False,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    print(result.stdout,end='',flush=True);print(result.stderr,end='',file=sys.stderr,flush=True)
    print('exit='+str(result.returncode),flush=True)
    if expected_stderr and expected_stderr not in result.stderr:
        print('FAIL: missing secondary close diagnostic '+expected_stderr,flush=True);return 1
    return result.returncode

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    if args.output:args.output.mkdir(parents=True,exist_ok=False)
    else:args.output=Path(tempfile.mkdtemp(prefix='installer-swap-config-',dir=os.environ.get('TMPDIR')))
    args.output=args.output.resolve();cc=shutil.which('clang');assert cc
    sources=['tools/tests/installer_swap_config_test.c']
    if (ROOT/'userland/apps/installer/installer_swap_config.c').exists():
        sources+=['userland/apps/installer/installer_swap_config.c']
    binary=str(args.output/'config')
    command=[cc,'-std=c11','-D_GNU_SOURCE','-O1','-g','-Wall','-Wextra','-Werror',
             '-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-pie','-no-pie',
             '-Wl,--wrap=renameat,--wrap=fsync,--wrap=symlinkat,--wrap=close,--wrap=fchown,--wrap=openat,--wrap=unlinkat',*sources,'-o',binary]
    result=run(command)
    if result:return result
    failures=0
    for case in ('enabled','disabled','file-sync','file-close','link-fail','fstab-rename',
                 'dir-sync','backup-unlink','dir-close','sync-close'):
        root=args.output/case
        for d in ('etc/conf.d','etc/runlevels/default','etc/init.d'):(root/d).mkdir(parents=True,exist_ok=True)
        (root/'etc/fstab').write_text('# old fstab\n')
        (root/'etc/conf.d/swap').write_text('original conf\n')
        (root/'etc/init.d/swap').write_text('stock\n')
        (root/'etc/init.d/reliefos-swap-check').write_text('check\n')
        failures+=bool(run([binary,case,str(root)],'configuration file close failed' if case=='sync-close' else None))
    return bool(failures)

if __name__=='__main__':raise SystemExit(main())
