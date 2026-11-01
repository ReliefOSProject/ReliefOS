#!/usr/bin/env python3
"""Run the complete production preparation with an actual host BusyBox mkswap."""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
def run(argv,env=None):
    print('+ '+shlex.join(argv),flush=True)
    ret=subprocess.run(argv,cwd=ROOT,env=env,check=False).returncode
    print('exit='+str(ret),flush=True);return ret
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path)
    p.add_argument('--busybox',type=Path,required=True);a=p.parse_args()
    if a.output:a.output.mkdir(parents=True,exist_ok=False)
    else:a.output=Path(tempfile.mkdtemp(prefix='installer-swap-full-',dir=os.environ.get('TMPDIR')))
    a.output=a.output.resolve();cc=shutil.which('clang');assert cc and a.busybox.is_file()
    bindir=a.output/'bin';bindir.mkdir();(bindir/'mkswap').symlink_to(a.busybox.resolve())
    binary=str(a.output/'test')
    cmd=[cc,'-std=c11','-D_GNU_SOURCE','-O1','-g','-Wall','-Wextra','-Werror',
         '-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-pie','-no-pie',
         '-ffunction-sections','-fdata-sections','-Wl,--gc-sections,--wrap=fchown,--wrap=fstat,--wrap=linkat,--wrap=renameat,--wrap=unlinkat,--wrap=fsync,--wrap=close',
         'tools/tests/installer_swap_transaction_test.c','userland/apps/installer/installer_swap.c',
         'userland/apps/installer/installer_swap_config.c','userland/apps/installer/installer_swap_prepare.c',
         'userland/apps/installer/installer_capacity.c','-o',binary]
    if run(cmd):return 1
    env=dict(os.environ,PATH=str(bindir)+':'+os.environ['PATH']);failures=0
    for case in ('success','disabled','fstab-failure','existing','post-commit-failure','rollback-failure',
                 'rollback-restored','rollback-close','preexisting-reference','bad-uuid'):
        root=a.output/case
        for d in ('etc/conf.d','etc/runlevels/default','etc/init.d'):(root/d).mkdir(parents=True,exist_ok=True)
        (root/'etc/fstab').write_text('/swapfile none swap sw 0 0\n' if case in ('preexisting-reference','bad-uuid') else '# old fstab\n')
        (root/'etc/conf.d/swap').write_text('original conf\n')
        (root/'etc/init.d/swap').write_text('stock\n');(root/'etc/init.d/reliefos-swap-check').write_text('check\n')
        if case=='existing':(root/'swapfile').write_text('keep')
        failures+=bool(run([binary,case,str(root)],env))
    return bool(failures)
if __name__=='__main__':raise SystemExit(main())
