#!/usr/bin/env python3
"""Boot the production VMDK snapshot and exercise public test/test GUI login.

Uses the public development-image account only. No authentication bypass,
filesystem injection, or writes to the source disk. Screenshots require review.
"""
from pathlib import Path
import argparse
import subprocess
import tempfile
import time
from test_sudo_e2e_qemu import Probe, type_line
from test_installer_window_qemu import wait_log
from test_xorg_vm_qemu import wait_greeter_stage
from run_gcc_probe_qemu import qmp_quit

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--smp', type=int, default=2)
    args = parser.parse_args()
    work = ROOT / 'build/openrc-gui'
    work.mkdir(parents=True, exist_ok=True)
    serial = work / f'guest-smp{args.smp}.log'
    serial.write_text('')
    with tempfile.TemporaryDirectory(prefix='reliefos-openrc-gui-') as directory:
        qmp = Path(directory) / 'qmp.sock'
        command = ['qemu-system-x86_64','-enable-kvm','-cpu','host','-machine','q35',
            '-m','4096','-smp',str(args.smp),'-bios','/usr/share/edk2/x64/OVMF.4m.fd',
            '-display','none','-serial',f'file:{serial}','-device','VGA,xres=1280,yres=720',
            '-netdev','user,id=n','-device','e1000,netdev=n',
            '-drive',f'file={ROOT / "build/images/reliefos.vmdk"},if=none,id=d,format=vmdk,snapshot=on',
            '-device','ich9-ahci,id=a','-device','ide-hd,drive=d,bus=a.0',
            '-qmp',f'unix:{qmp},server=on,wait=off','-no-reboot','-no-shutdown']
        with (work/'qemu.log').open('w') as log:
            process = subprocess.Popen(command,stdout=log,stderr=subprocess.STDOUT)
            probe = None
            try:
                deadline = time.monotonic() + 10
                while not qmp.exists():
                    if process.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError("QEMU QMP socket did not become ready")
                    time.sleep(.05)
                probe = Probe(qmp,work)
                wait_greeter_stage(probe, process, 'login', 'login-ready', timeout=90)
                probe.frame('login')
                probe.key('down')
                probe.text('test')
                probe.key('ret')
                # The login process intentionally owns the PAM session until logout.
                time.sleep(8)
                probe.frame('desktop')
                probe.key('meta_l'); time.sleep(1)
                probe.text('terminal'); probe.key('ret')
                time.sleep(3)
                type_line(probe, 'echo ST_OPENRC_READY >/dev/ttyS0', settle=0.5)
                wait_log(serial, 'ST_OPENRC_READY', process, timeout=30)
                type_line(probe,'id; rc-status; cat /run/reliefos/dhcp-lease',settle=4)
                probe.frame('terminal-openrc')
                print(f'GUI login + Terminal launched; review {work}/terminal-openrc.png')
            finally:
                if probe:
                    probe.frame('final')
                    probe.close()
                qmp_quit(qmp,process)

if __name__ == '__main__': main()
