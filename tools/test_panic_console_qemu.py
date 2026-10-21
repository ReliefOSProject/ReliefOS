#!/usr/bin/env python3
"""Observe real kernel page-fault panic on a text VT or running Xorg desktop.

Only a private raw copy is changed. GDB injects one faulting instruction into
that VM's RAM, after normal boot; no panic trigger is added to the product.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import time
import unicodedata

from PIL import Image
from run_gcc_probe_qemu import qmp_quit
from test_boot_logo_qemu import digest, run
from test_installer_accounts_qemu import Probe
from test_installer_window_qemu import wait_log
from test_kbuild_qemu import read_guest
from test_vt_qemu import wait_text, ocr
import test_vdso_qemu as disk
import test_xorg_vm_qemu as xorg

ROOT = Path(__file__).resolve().parents[1]

def inspect_overlay(before, after, report):
    """Check actual pixels against terminal cells derived from the serial report."""
    if before.size != after.size:
        raise AssertionError('panic changed the display mode')
    width, height = after.size
    columns, rows = width//8, height//16
    occupied = [0]*rows
    row = column = 0
    plain = re.sub(r'\x1b\[[0-9;]*m', '', report).replace('\r', '')
    for ch in plain:
        if row >= rows: break
        if ch == '\n':
            row += 1
            column = 0
            continue
        cells = 2 if unicodedata.east_asian_width(ch) in ('W', 'F') else 1
        if cells > columns: break
        if column+cells > columns:
            row += 1
            column = 0
        if row >= rows: break
        column += cells
        occupied[row] = max(occupied[row], column)
    original, actual = before.convert('RGB').tobytes(), after.convert('RGB').tobytes()
    palette = {(0,0,0), (255,85,85), (255,255,85), (255,255,255)}
    untouched = visible_desktop = 0
    for y in range(height):
        edge = occupied[y//16]*8 if y//16 < rows else 0
        start, end = (y*width+edge)*3, (y+1)*width*3
        if original[start:end] != actual[start:end]:
            raise AssertionError(f'GUI pixels outside panic text changed on row {y}, after x={edge}')
        untouched += width-edge
        visible_desktop += sum(original[p:p+3] != b'\0\0\0' for p in range(start,end,3))
        for x in range(edge):
            offset = (y*width+x)*3
            if tuple(actual[offset:offset+3]) not in palette:
                raise AssertionError(f'GUI remains inside panic text cell at {x},{y}')
    if visible_desktop < 10000:
        raise AssertionError('scenario needs visible GUI pixels outside the text')
    return {'unchanged_pixels_outside_text': untouched,
            'preserved_nonblack_gui_pixels': visible_desktop}


def prepare(source, kernel, output):
    """Clone RAW, merge the public test user, and verify injected kernel bytes."""
    raw = output / 'guest.raw'
    run(['cp', '--reflink=auto', '--sparse=always', source, raw])
    partitions = disk.gpt_partitions(raw)
    esp = next(p for p in partitions if p['name'] == 'RELIEFOS_ESP')
    root = next(p for p in partitions if p['name'] == 'RELIEFOS_ROOT')
    layout = {'root_first': int(root['first']), 'root_last': int(root['last'])}
    filesystem = disk.extract_root(raw, layout, output)
    commands = []
    disk.add_missing_root_directory(filesystem, '/home/test', commands)
    commands.extend(['set_inode_field /home/test uid 1000',
                     'set_inode_field /home/test gid 1000',
                     'set_inode_field /home/test mode 040700'])
    for name in ('passwd', 'shadow', 'group', 'gshadow'):
        path = output / name
        read_guest(filesystem, '/etc/' + name, path)
        lines = [s for s in path.read_text().splitlines() if not s.startswith('test:')]
        lines.extend(s for s in (ROOT/'system/test-accounts'/name).read_text().splitlines()
                     if s.startswith('test:'))
        path.write_text('\n'.join(lines) + '\n')
        path.chmod(0o600 if 'shadow' in name else 0o644)
        commands.extend([f'rm /etc/{name}', f'write "{path}" /etc/{name}',
                         f'set_inode_field /etc/{name} mode ' + ('0100600' if 'shadow' in name else '0100644')])
    script = output/'accounts.commands'
    script.write_text('\n'.join(commands)+'\n')
    with (output/'inject.log').open('w') as log:
        run(['debugfs', '-w', '-f', script, filesystem], stdout=log, stderr=subprocess.STDOUT)
    disk.put_root_back(raw, filesystem, layout)
    spec = f'{raw}@@{int(esp["first"])*512}'
    run(['mcopy', '-o', '-i', spec, kernel, '::/reliefos/kernel.sys'])
    readback = output/'shipped-kernel.sys'
    run(['mcopy', '-i', spec, '::/reliefos/kernel.sys', readback])
    if digest(readback) != digest(kernel): raise AssertionError('private kernel readback mismatch')
    return raw


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--image', type=Path, help='RAW base; defaults to build images/reliefos.raw')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--mode', choices=('tty', 'gui'), required=True)
    parser.add_argument('--hold-display-locks', action='store_true', help='also simulate interrupted console/SVGA lock owners')
    args = parser.parse_args()
    build, output = args.build_dir.resolve(), args.output.resolve()
    source = args.image.resolve() if args.image else build/'images/reliefos.raw'
    if not output.is_relative_to(ROOT/'out') or output.exists():
        parser.error('--output must be a new directory below out/')
    if len(str(output/'gdb.sock').encode()) >= 108: parser.error('use a shorter output path')
    output.mkdir(parents=True)
    (ROOT/'out/tmp').mkdir(parents=True, exist_ok=True)
    os.environ['TMPDIR'] = str(ROOT/'out/tmp')
    kernel = build/'generated/system/kernel.sys'
    symbols = build/'reliefnt/generated/system/kernel.unstripped'
    source_hash = digest(source)
    metadata = {'mode': args.mode, 'platform': 'QEMU/KVM q35 SVGA II, 2 CPUs, 4 GiB',
                'kernel_sha256': digest(kernel), 'symbols_sha256': digest(symbols),
                'source_image': str(source), 'source_sha256': source_hash,
                'injection': 'GDB writes mov (%rax),%rax at console_display_tick in guest RAM; RAX=0xffff800000000000',
                'held_display_locks': args.hold_display_locks, 'vmware_verified': False}
    process = probe = None
    try:
        raw = prepare(source, kernel, output)
        serial, qmp, gdb = (output/name for name in ('serial.log', 'qmp.sock', 'gdb.sock'))
        command = ['qemu-system-x86_64', '-enable-kvm', '-cpu', 'host', '-machine', 'q35',
                   '-m', '4096', '-smp', '2', '-bios', '/usr/share/edk2/x64/OVMF.4m.fd',
                   '-display', 'none', '-serial', f'file:{serial}', '-vga', 'vmware',
                   '-device', 'qemu-xhci', '-device', 'usb-tablet', '-netdev', 'user,id=net0',
                   '-device', 'e1000,netdev=net0', '-drive', f'file={raw},format=raw,if=ide,snapshot=on',
                   '-qmp', f'unix:{qmp},server=on,wait=off',
                   '-chardev', f'socket,path={gdb},server=on,wait=off,id=gdb0',
                   '-gdb', 'chardev:gdb0', '-no-reboot', '-no-shutdown']
        (output/'qemu-command.json').write_text(json.dumps(command, indent=2)+'\n')
        with (output/'qemu.log').open('w') as log:
            process = subprocess.Popen(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
        deadline = time.monotonic()+15
        while not qmp.exists():
            if process.poll() is not None or time.monotonic()>deadline: raise RuntimeError('QEMU startup failed')
            time.sleep(.1)
        probe = Probe(qmp, output)
        wait_log(serial, 'boot complete:', process, 120)
        xorg.wait_greeter_stage(probe, process, 'login', 'xdm-ready', timeout=120)
        if args.mode == 'tty':
            xorg.switch_vt(probe, serial, process, 2)
            wait_text(probe, process, 'tty2-ready', 'login:', timeout=30)
        else:
            xorg.greeter_login(probe, process)
            xorg.wait_session(probe, process, 'icewm-ready')
            xorg.st_marker(probe, serial, process, 'PANIC-GUI-READY')
        before = probe.frame('before-panic')
        before.save(output/'before-panic.png')
        text = serial.read_text(errors='replace')
        if re.search(r'KERNEL PANIC|\[bugcheck\]', text): raise AssertionError('panic occurred before injection')
        metadata['normal_boot_and_display_ready'] = True
        script = output/'inject.gdb'
        gdb_commands = ['set pagination off', 'set confirm off', f'target remote {gdb}',
                        'hbreak console_display_tick', 'continue',
                        'printf "FAULT-INJECTION RIP=%#lx CS=%#lx RSP=%#lx\\n", $rip, $cs, $rsp',
                        'x/8bx $rip', 'delete breakpoints']
        if args.hold_display_locks:
            gdb_commands += ['set {unsigned int} &console_log_owner = 0xfffffffe',
                             'set {unsigned int} &device_lock = 1']
        gdb_commands += ['set {unsigned char} (unsigned long)&console_display_tick = 0x48',
                         'set {unsigned char} ((unsigned long)&console_display_tick+1) = 0x8b',
                         'set {unsigned char} ((unsigned long)&console_display_tick+2) = 0x00',
                         'set $rax = 0xffff800000000000',
                         'hbreak console_panic_begin', 'continue',
                         f'monitor screendump {output}/frozen-display.ppm',
                         'delete breakpoints', 'detach', 'quit', '']
        script.write_text('\n'.join(gdb_commands))
        print(f'Injecting real kernel page fault while {args.mode} is visible', flush=True)
        with (output/'gdb.log').open('w') as log:
            run(['gdb', '-q', '-nx', '--batch', symbols, '-x', script],
                stdout=log, stderr=subprocess.STDOUT, timeout=30)
        wait_log(serial, 'RESTART REQUIRED', process, 30)
        first = probe.frame('panic')
        first.save(output/'panic.png')
        time.sleep(2)
        late = probe.frame('panic-stable')
        late.save(output/'panic-stable.png')
        if first.tobytes() != late.tobytes(): raise AssertionError('panic display was overwritten after halt')
        final_log = serial.read_text(errors='replace')
        report = final_log[final_log.index('!!! KERNEL PANIC'):]
        (output/'panic-report.txt').write_text(report)
        for needle in ('Unhandled Page Fault', 'CR2=0xFFFF800000000000', 'RAX=', 'RIP=',
                       'PF: P=0 W=0 U=0', 'other CPUs stopped=1/1',
                       'Display: tty'+('2 (current text console)' if args.mode=='tty' else '1 (GUI -> panic text overlay)')):
            if needle not in report: raise AssertionError('missing diagnostic: '+needle)
        visible = ocr(first, psm=6)
        (output/'panic.ocr.txt').write_text(visible)
        if 'KERNEL PANIC' not in visible or 'CR2' not in visible:
            raise AssertionError('fatal diagnostics not readable in actual screenshot')
        if args.mode == 'gui':
            pixels = first.convert('RGB')
            red = sum((lambda rgb: rgb[0]>200 and rgb[1]<150 and rgb[2]<150)(pixels.getpixel((x,y)))
                      for y in range(16) for x in range(pixels.width))
            if red<100: raise AssertionError('headline did not start on screen row zero')
            with Image.open(output/'frozen-display.ppm') as frozen:
                frozen.save(output/'frozen-display.png')
                metadata.update(inspect_overlay(frozen, first, report))
        metadata.update(passed=True, screenshot=str(output/'panic.png'), stable_after_halt=True,
                        panic_report=report, actual_screenshot_ocr=visible)
        print('PASS current VT, detailed registers, stopped peer CPU, readable stable panic text with unchanged GUI outside glyph cells', flush=True)
    except Exception as error:
        metadata.update(passed=False, error=str(error))
        raise
    finally:
        if probe is not None: probe.close()
        if process is not None and process.poll() is None: qmp_quit(output/'qmp.sock', process)
        metadata['source_image_unchanged'] = digest(source)==source_hash
        (output/'result.json').write_text(json.dumps(metadata, ensure_ascii=False, indent=2)+'\n')
        if not metadata['source_image_unchanged']: raise AssertionError('base image changed')


if __name__ == '__main__':
    main()
