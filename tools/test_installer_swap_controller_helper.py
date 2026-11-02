#!/usr/bin/env python3
"""Check QEMU helper defaults and explicit TTY swap prompts without a VM."""
import argparse
import ast
import inspect
import sys
from pathlib import Path
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import test_installer_accounts_qemu as helper

class Probe:
    def __init__(self):self.answers=[]
    def text(self,text):self.answers.append(text)
    def key(self,key):assert key=='ret'

def run_case(size):
    probe=Probe();prompts=[]
    def wait(serial,prompt,process,timeout):prompts.append(prompt)
    with patch.object(helper,'wait_log',wait),patch.object(helper,'wait_install') as finish:
        helper.install_tty(probe,None,None,select_entry=False,swap_mib=size)
        finish.assert_called_once()
    assert prompts[2:4]==['Install or update BIOS bootloader? [Y/n]: ',
                          'Install or update UEFI bootloader? [Y/n]: ']
    assert prompts[4]=='partition> '
    assert prompts[11]=='Install Xorg desktop environment? [Y/n]: '
    assert prompts[12]=='Create a swapfile? [y/N]: '
    assert probe.answers[12]==('y' if size is not None else '')
    if size is not None:
        assert prompts[13]=='Swap size in MiB [' and probe.answers[13]==str(size)
    else:assert not any(p.startswith('Swap size in MiB [') for p in prompts)
    assert prompts[-1]=='Type INSTALL to confirm the partition layout and installation: '
    print('PASS TTY helper swap_mib='+str(size))

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    if args.output:args.output.mkdir(parents=True,exist_ok=False)
    for name in ('tools/test_installer_accounts_qemu.py','tools/test_dual_boot_qemu.py'):
        ast.parse((ROOT/name).read_text(),filename=name)
    assert inspect.signature(helper.install_gui).parameters['swap_mib'].default is None
    assert inspect.signature(helper.install_tty).parameters['swap_mib'].default is None
    run_case(None);run_case(512)
    try:helper.install_tty(Probe(),None,None,select_entry=False,swap_mib=0)
    except ValueError:pass
    else:raise AssertionError('invalid requested swap size accepted')
    print('PASS GUI/TTY helper signatures and invalid-size refusal')
    return 0
if __name__=='__main__':raise SystemExit(main())
