#!/usr/bin/env python3
"""Exercise installer VT switching and optional installation on a new scratch disk."""
import argparse
from pathlib import Path
import re
import subprocess
import time

from test_installer_accounts_qemu import boot, next_page
from test_installer_window_qemu import wait_log
from test_vt_qemu import wait_text as wait_screen, ocr

ROOT = Path(__file__).resolve().parents[1]


def wait_text(probe, process, name, needle, timeout=30):
    return wait_screen(probe, process, name, needle, timeout, psm=11)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--iso', type=Path, default=ROOT / 'out/x86_64/release/images/reliefos-installer.iso')
    parser.add_argument('--tui', action='store_true')
    parser.add_argument('--install', action='store_true')
    parser.add_argument('--exit-recovery', action='store_true',
                        help='Kill the installer session from another VT and '
                             'verify recovery (mutually exclusive with --install)')
    # The install copy phase runs as one long busybox pass; on slow hosts the
    # 20-minute default truncates a healthy install at ~85% progress. Override
    # per run (e.g. --install-timeout 3600) instead of silently stretching it.
    parser.add_argument('--install-timeout', type=int, default=1200)
    args = parser.parse_args()
    if args.install and args.exit_recovery:
        parser.error('--install and --exit-recovery are mutually exclusive')
    args.output.mkdir(parents=True, exist_ok=False)
    disk = args.output.resolve() / 'scratch.raw'
    subprocess.run(['qemu-img', 'create', '-f', 'raw', str(disk), '4G'], check=True)
    with boot(args.output.resolve(), disk, args.iso.resolve()) as (probe, serial, process):
        if args.tui:
            time.sleep(3)
            probe.key('down')
            probe.key('ret')
            wait_text(probe, process, 'installer-shell', 'built-in shell', timeout=100)
        else:
            wait_text(probe, process, 'installer-language', 'English', timeout=100)
            probe.click(*probe.language_option_point())
            wait_text(probe, process, 'installer-english', 'Select Language')
        for number in range(2, 7):
            probe.key(f'ctrl-alt-f{number}')
            wait_text(probe, process, f'installer-tty{number}', 'built-in shell')
        probe.key('ctrl-alt-f1')
        if args.tui:
            probe.text('/usr/lib/reliefos/apps/installer/installer.elf')
            probe.key('ret')
            wait_text(probe, process, 'installer-tui-mode', 'Mode [install/update')
            probe.text('install'); probe.key('ret')
            wait_text(probe, process, 'installer-tui-disk', 'Select disk number')
            probe.text('q'); probe.key('ret')
            # Installer exit/recovery: quitting the TUI installer must return
            # the rescue shell to interactive use, and an exited shell must be
            # respawned by init on the same VT. A serial-only echo proves which
            # process owns the keyboard; stale OCR of the old prompt cannot.
            time.sleep(2)
            probe.text('echo TUI-EXIT-OK >/dev/ttyS0')
            probe.key('ret')
            wait_log(serial, 'TUI-EXIT-OK', process, 20)
            before = serial.read_text().count(
                'path=/usr/lib/reliefos/apps/login/login.elf')
            probe.text('exit')
            probe.key('ret')
            deadline = time.monotonic() + 30
            while serial.read_text().count(
                    'path=/usr/lib/reliefos/apps/login/login.elf') <= before:
                assert process.poll() is None and time.monotonic() < deadline, \
                    'the installer shell did not respawn after exit'
                time.sleep(.2)
            probe.text('echo TUI-RESPAWN-OK >/dev/ttyS0')
            probe.key('ret')
            wait_log(serial, 'TUI-RESPAWN-OK', process, 20)
            print('PASS installer TUI, tty1–tty6 shells and exit/recovery', flush=True)
            return
        wait_text(probe, process, 'installer-restored', 'Select Language')
        if args.exit_recovery:
            # Installer session exit/recovery, GUI side: with the installer
            # graphical session running on tty1, killing it from the rescue
            # shell on tty2 must recover tty1 to that rescue shell (the
            # installer-media fallthrough in console-session).
            probe.key('ctrl-alt-f2')
            wait_text(probe, process, 'exit-recovery-shell', 'built-in shell')
            match = re.search(
                r'exec pid=(\d+) path=/usr/bin/Xorg',
                serial.read_text())
            assert match, 'graphical session pid not recorded'
            probe.text(f'kill {match[1]}')
            probe.key('ret')
            time.sleep(2)
            probe.frame('installer-session-killed')
            probe.key('ctrl-alt-f1')
            wait_text(probe, process, 'installer-exit-recovered', 'built-in shell',
                      timeout=40)
            print('PASS installer session exit/recovery to the rescue shell', flush=True)
            return
        for _ in range(6): next_page(probe)
        wait_text(probe, process, 'installer-accounts', 'Standard user name')
        if args.install:
            # The accounts page initially focuses the username. Keep field
            # changes and typing on the same keyboard queue; USB pointer and
            # PS/2 keyboard commands have independent delivery order.
            for index, text in enumerate(('alice', 'password', 'password', 'rootpass', 'rootpass')):
                probe.text(text)
                if index == 0:
                    wait_text(probe, process, 'installer-username', 'alice')
                if index < 4:
                    probe.key('tab')
            next_page(probe)
            wait_text(probe, process, 'installer-confirm', 'INSTALL')
            probe.key('caps_lock')
            probe.text('install')
            probe.key('caps_lock')
            probe.frame('installer-confirm-typed')
            next_page(probe)
            deadline = time.monotonic() + args.install_timeout
            while time.monotonic() < deadline:
                visible = ocr(probe.frame('installer-progress'), psm=11)
                if 'Installation finished' in visible or 'Installation Complete' in visible:
                    print('PASS installer completed on scratch disk', flush=True)
                    break
                if 'failed' in visible.lower(): raise AssertionError(visible)
                assert process.poll() is None
                time.sleep(5)
            else: raise AssertionError('Installation timed out')
        print(f'PASS installer GUI, VT restore and account input: {disk}', flush=True)


if __name__ == '__main__':
    main()
