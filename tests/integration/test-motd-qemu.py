#!/usr/bin/env python3
"""Run the MOTD/PAM-topology probe on ReliefNT using an isolated diagnostic ISO.
First build: make O=out image-vmdk iso
Then run: python3 tests/integration/test-motd-qemu.py out
No production image is modified; the existing ioctl regression launch slot is
used only in a private copy to avoid introducing a new kernel test hook.
"""
import os
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("out", type=Path)
parser.add_argument("--root", type=Path, help="root image; defaults to root.ext4, then root.ext2")
args = parser.parse_args()
src = Path(__file__).resolve().parents[2]
out = args.out.resolve()
log = out / 'logs/motd-qemu.log'
firmware = next((Path(p) for p in ('/usr/share/edk2/x64/OVMF.4m.fd',
    '/usr/share/OVMF/OVMF_CODE_4M.fd', '/usr/share/ovmf/OVMF.fd') if Path(p).exists()), None)
assert firmware, 'OVMF firmware is required'

def run(args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)

with tempfile.TemporaryDirectory(prefix='reliefos-motd-qemu-') as directory:
    work = Path(directory)
    probe = work / 'probe.elf'
    run(['clang', '--target=x86_64-linux-musl', f'--sysroot={out}/sysroot/musl',
         '--gcc-toolchain=/nonexistent', '-fuse-ld=lld', '--rtlib=compiler-rt',
         '--unwindlib=none', '-static', '-O2', '-Wall', '-Wextra', '-Werror',
         src / 'tools/tests/motd_guest_probe.c', '-o', probe])
    source_root = args.root.resolve() if args.root else next(
        (candidate for candidate in (out / 'images/root.ext4', out / 'images/root.ext2') if candidate.exists()),
        None,
    )
    assert source_root and source_root.exists(), 'an ext4 or ext2 root image is required'
    root = work / source_root.name
    shutil.copyfile(source_root, root)
    guest = '/usr/lib/reliefos/tests/linux-ioctl-cloexec.elf'
    for command in ('mkdir /usr/lib/reliefos/tests', f'rm {guest}',
                    f'write {probe} {guest}', f'set_inode_field {guest} mode 0100755'):
        run(['debugfs', '-w', '-R', command, root], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    config = work / 'grub.cfg'
    config.write_text((src / 'boot/grub/live.cfg').read_text().replace('set timeout=5', 'set timeout=0')
                      .replace('bootlog=1', '').replace('mode=live', 'mode=live autospawn=ioctlcloexec'))
    iso = work / 'motd.iso'
    with (work / 'iso.log').open('w') as build_log:
        run(['sh', src / 'tools/build/iso.sh', src, out / 'grub-efi/lib/grub/x86_64-efi',
             out / 'stage/esp', root, config, iso, '1790100000', 'RELIEFOSMOTD',
             out / 'grub-host/lib/grub/i386-pc', out / 'grub-host/bin/grub-mkimage'],
            env=dict(os.environ, GRUB_MKSTANDALONE=str(out / 'grub-host/bin/grub-mkstandalone')),
            stdout=build_log, stderr=subprocess.STDOUT)
    args = ['qemu-system-x86_64', '-machine', 'q35', '-m', '2048', '-smp', '2',
            '-bios', str(firmware), '-display', 'none', '-serial', f'file:{log}',
            '-cdrom', str(iso), '-boot', 'd', '-no-reboot',
            '-device', 'VGA', '-netdev', 'user,id=net0', '-device', 'e1000,netdev=net0']
    args += ['-enable-kvm', '-cpu', 'host'] if os.access('/dev/kvm', os.R_OK | os.W_OK) else ['-cpu', 'max']
    log.write_text('')
    with (out / 'logs/motd-qemu-stderr.log').open('w') as errors:
        process = subprocess.Popen(args, stderr=errors, stdout=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + 180
            while time.monotonic() < deadline:
                text = log.read_text(errors='replace') if log.exists() else ''
                if '!!!! X64 Exception' in text:
                    raise RuntimeError(f'Firmware/GRUB failed before kernel startup: see {log}')
                if '[motd-test] DONE' in text:
                    assert '[motd-test] DONE failures=0' in text, text[text.find('[motd-test] BEGIN'):]
                    print(text[text.find('[motd-test] BEGIN'):])
                    print(f'PASS ReliefNT: PTY width, both locales, sysinfo load and .hushlogin; {log}')
                    break
                if process.poll() is not None:
                    raise RuntimeError(f'QEMU exited: see {log}')
                time.sleep(1)
            else:
                raise TimeoutError(f'MOTD guest probe timed out: see {log}')
        finally:
            if process.poll() is None:
                process.terminate()
                try: process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait()
