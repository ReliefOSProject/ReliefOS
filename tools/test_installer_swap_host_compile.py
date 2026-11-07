#!/usr/bin/env python3
"""Compile changed installer C and GUI translation units as host objects."""
import argparse
from pathlib import Path
import shlex
import shutil
import subprocess
ROOT=Path(__file__).resolve().parents[1]
def run(argv):
    print('+ '+shlex.join(argv),flush=True)
    rc=subprocess.run(argv,cwd=ROOT,check=False).returncode
    print('exit='+str(rc),flush=True);return rc
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--fltk-include',type=Path,required=True);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    cc=shutil.which('clang');cxx=shutil.which('clang++');assert cc and cxx
    base_headers=['-idirafter','userland/runtime/include','-idirafter','include']
    headers=base_headers+['-Ikernel/reliefnt/include','-Ikernel/reliefnt/include/uapi']
    compat=(a.output/'host-mount-compat.h').resolve()
    compat.write_text('#include <linux/mount.h>\n#undef MNT_FORCE\n#undef MNT_DETACH\n'
                      '#undef MNT_EXPIRE\n#undef UMOUNT_NOFOLLOW\n')
    sources=['install_ops.c','installer_capacity.c','installer_swap.c','installer_swap_bundle.c',
             'installer_swap_config.c','installer_swap_prepare.c','installer_swap_storage.c','installer_tty.c']
    for name in sources:
        selected_headers=base_headers if name=='installer_swap_storage.c' else headers
        cmd=[cc,'-std=gnu11','-O1','-Wall','-Wextra',*selected_headers,'-include',str(compat),'-c',
             'userland/apps/installer/'+name,'-o',str((a.output/name.replace('.c','.o')).resolve())]
        if run(cmd):return 1
    cmd=[cxx,'-std=gnu++17','-O1','-Wall','-Wextra','-isystem',str(a.fltk_include.resolve()),*headers,
         '-c','userland/apps/installer/main.cpp','-o',str((a.output/'main.o').resolve())]
    if run(cmd):return 1
    print('PASS changed console/backend and FLTK source units compiled as host objects')
    return 0
if __name__=='__main__':raise SystemExit(main())
