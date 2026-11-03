#!/usr/bin/env python3
"""Exercise a two-tier xHCI Hub tree in a disposable packaged OS snapshot.

QEMU's usb-hub is full-speed. This proves real downstream BOT/boot-HID and
removal; HS TT and SS Hub wire/context semantics have separate kernel tests.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time
import uuid

from test_installer_accounts_qemu import Probe
from test_kernel_modules_qemu import digest
from test_vt_qemu import wait_text
from test_usb2_qemu import (PAYLOAD, PAYLOAD_HASH, make_stick, payload_server,
                            firmware_options, wait_marker, wait_root, xhci_guest_devices)
from test_installer_window_qemu import wait_log

ROOT = Path(__file__).resolve().parents[1]


def tree_devices(controller):
    """Keep the keyboard/mouse two hubs deep and a disk on a separate Hub port."""
    if controller not in ('qemu-xhci', 'nec-usb-xhci'):
        raise ValueError('unsupported xHCI model')
    return [f'{controller},id=xhci,p2=8,p3=8',
            'usb-hub,id=hub0,bus=xhci.0,port=1,ports=4',
            'usb-hub,id=hub1,bus=xhci.0,port=1.2,ports=4',
            'usb-kbd,id=kbd,bus=xhci.0,port=1.2.1,usb_version=1',
            'usb-mouse,id=mouse,bus=xhci.0,port=1.2.2,usb_version=1',
            'usb-storage,id=flash,bus=xhci.0,port=1.3,drive=stick']


def hotplug_arguments(description):
    """Preserve dotted port strings and use QMP integers for numeric properties."""
    parts = description.split(',')
    values = dict(part.split('=', 1) for part in parts[1:])
    for key in ('ports', 'usb_version'):
        if key in values:
            values[key] = int(values[key])
    return {'driver': parts[0], **values}


def evidence(usb, serial, *, after=None):
    """Match each QMP port/address to its own route, root port and parent slot."""
    if after:
        boundaries = list(re.finditer(r'(?m)^' + re.escape(after) + r'\r?$', serial))
        if len(boundaries) != 1:
            raise AssertionError('missing or duplicate enumeration boundary: ' + after)
        serial = serial[boundaries[0].end():]
    physical = {}
    for line in usb.splitlines():
        match = re.fullmatch(r'\s*Device (\d+)\.(\d+), Port (\d+(?:\.\d+)*), '
                             r'Speed (\d+) Mb/s, Product (.*?), ID: (\S+)\s*', line)
        if not match:
            continue
        bus, address, port, mbps, product, name = match.groups()
        if name in physical:
            raise AssertionError('duplicate physical USB identity')
        physical[name] = dict(bus=int(bus), address=int(address), port=port,
                              mbps=int(mbps), product=product)
    guest = xhci_guest_devices(serial)
    topology = {}
    for line in serial.splitlines():
        match = re.search(r'\[usb\] xHCI device slot=(\d+) .* route=([0-9a-f]{5}) parent=(\d+) port=(\d+)\s*$', line)
        if match:
            slot, route, parent, port = match.groups()
            topology[int(slot)] = (int(route, 16), int(parent), int(port))
    expected = {'hub0': ('1', 0, None, '0409', '55aa'),
                'hub1': ('1.2', 2, 'hub0', '0409', '55aa'),
                'kbd': ('1.2.1', 0x12, 'hub1', '0627', '0001'),
                'mouse': ('1.2.2', 0x22, 'hub1', '0627', '0001'),
                'flash': ('1.3', 3, 'hub0', '46f4', '0001')}
    found = {}
    for name, (port, route, parent, vid, pid) in expected.items():
        item = physical.get(name)
        if not item or (item['port'], item['mbps']) != (port, 12):
            raise AssertionError('physical Hub path/speed mismatch: ' + name)
        candidates = [d for d in guest if d['root_port'] == 9 and d['address'] == item['address']
                      and topology.get(d['slot'], (None,))[0] == route]
        if not candidates:
            raise AssertionError('missing routed guest device: ' + name)
        child = candidates[-1]
        if (child['speed'], child['bps'], child['vid'], child['pid']) != ('FULL', 12000000, vid, pid):
            raise AssertionError('guest Hub identity/speed mismatch: ' + name)
        expected_parent = found[parent]['guest']['slot'] if parent else 0
        if topology[child['slot']][1:] != (expected_parent, int(port.split('.')[-1]) if parent else 0):
            raise AssertionError('wrong parent slot/port: ' + name)
        found[name] = {'qemu': item, 'guest': child, 'route': route}
    return found


def guest_script(base, probe_hash, release, modular, nonce, module):
    """Read/write real FAT media, retain old FDs and resume after whole-tree replacement."""
    script = f'''#!/bin/sh
exec >/dev/ttyS0 2>&1
set -eu
trap 'result=$?; echo USB_QA_EXIT:{nonce}:$result' EXIT
test "$(uname -r)" = '{release}'
echo HUB_KEYBOARD_LOGIN_OK
wget -qO /tmp/usb-probe {base}/p
echo '{probe_hash}  /tmp/usb-probe' | sha256sum -c -
chmod 700 /tmp/usb-probe
mkdir -p /tmp/hub-stick
mount_stick() {{
 for candidate in /dev/sd*; do
  test -b "$candidate" || continue
  mount -t vfat "$candidate" /tmp/hub-stick 2>/dev/null || continue
  if test "$(cat /tmp/hub-stick/fixture.txt)" = stick; then
   stick=$candidate; return 0
  fi
  umount /tmp/hub-stick
 done
 return 1
}}
mount_stick
echo '{PAYLOAD_HASH}  /tmp/hub-stick/payload.bin' | sha256sum -c -
cp /tmp/hub-stick/payload.bin /tmp/hub-stick/copied.bin
sync
echo '{PAYLOAD_HASH}  /tmp/hub-stick/copied.bin' | sha256sum -c -
umount /tmp/hub-stick
echo HUB_STORAGE_RW_OK
if test -f /run/xdm.pid; then
 xdm_pid=$(cat /run/xdm.pid)
 kill "$xdm_pid"
 tries=0
 while kill -0 "$xdm_pid" 2>/dev/null; do
  tries=$((tries+1)); test "$tries" -lt 15; sleep 1
 done
fi
/tmp/usb-probe mouse
/tmp/usb-probe block "$stick" >/tmp/old-usb.log 2>&1 &
holder=$!
until grep -q USB_OLD_FD_READY /tmp/old-usb.log; do sleep 1; done
echo HUB_DISK_REMOVE_READY
wait "$holder"
cat /tmp/old-usb.log
grep -q USB_OLD_FD_ENODEV /tmp/old-usb.log
tries=0
until mount_stick; do tries=$((tries+1)); test "$tries" -lt 30; sleep 1; done
echo '{PAYLOAD_HASH}  /tmp/hub-stick/copied.bin' | sha256sum -c -
umount /tmp/hub-stick
echo HUB_DISK_REPLUG_OK
/tmp/usb-probe block "$stick" >/tmp/old-tree.log 2>&1 &
holder=$!
until grep -q USB_OLD_FD_READY /tmp/old-tree.log; do sleep 1; done
echo HUB_TREE_REMOVE_READY
wait "$holder"
cat /tmp/old-tree.log
grep -q USB_OLD_FD_ENODEV /tmp/old-tree.log
tries=0
until mount_stick; do tries=$((tries+1)); test "$tries" -lt 30; sleep 1; done
echo '{PAYLOAD_HASH}  /tmp/hub-stick/copied.bin' | sha256sum -c -
echo HUB_TREE_REPLUG_OK
'''
    if modular:
        script += f'''if rmmod {module}; then exit 42; fi
echo HUB_MODULE_BUSY_OK
umount /tmp/hub-stick
rmmod {module}
echo HUB_MODULE_UNLOADED
modprobe {module}
tries=0
until mount_stick; do tries=$((tries+1)); test "$tries" -lt 30; sleep 1; done
echo '{PAYLOAD_HASH}  /tmp/hub-stick/copied.bin' | sha256sum -c -
echo HUB_MODULE_RELOADED
'''
    script += f'umount /tmp/hub-stick\necho HUB_QA_OK:{nonce}\n'
    return script.encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--controller', choices=('qemu-xhci', 'nec-usb-xhci'), default='qemu-xhci')
    parser.add_argument('--firmware', choices=('bios', 'uefi'), default='bios')
    parser.add_argument('--modules', action='store_true')
    args = parser.parse_args()
    image = args.image.resolve();output = args.output.resolve()
    release = json.loads(args.manifest.read_bytes())['release']
    if not image.is_file() or not re.fullmatch(r'[A-Za-z0-9._+-]{1,31}', release):
        parser.error('regular packaged image and valid manifest required')
    output.mkdir(parents=True, exist_ok=False)
    nonce = uuid.uuid4().hex[:10]
    socket = ROOT.parent / ('.hub-' + nonce + '.sock')
    if socket.exists() or len(os.fsencode(socket)) >= 104:
        raise ValueError('owned QMP path unavailable')
    stick = make_stick(output, 'stick', 'whole')
    probe_bytes = args.probe.read_bytes()
    result = {'passed': False, 'source_sha256': digest(image), 'release': release,
              'probe_sha256': hashlib.sha256(probe_bytes).hexdigest(), 'nonce': nonce,
              'controller': args.controller, 'firmware': args.firmware, 'modules': args.modules}
    serial = output / 'serial.log'
    command = ['qemu-system-x86_64', '-machine', 'q35,i8042=off', '-accel', 'kvm', '-cpu', 'host',
               '-m', '4096', '-smp', '2', '-display', 'none', '-serial', 'file:' + str(serial),
               '-vga', 'vmware', '-netdev', 'user,id=n', '-device', 'e1000,netdev=n',
               '-drive', f'file={image},format=raw,if=none,id=os,snapshot=on',
               '-device', 'nvme,drive=os,serial=xhcihub,bootindex=1',
               '-drive', f'file={stick},format=raw,if=none,id=stick',
               '-qmp', f'unix:{socket},server=on,wait=off', '-no-reboot', '-no-shutdown',
               *firmware_options(args.firmware)]
    for device in tree_devices(args.controller):
        command += ['-device', device]
    result['command'] = command
    probe = process = None
    try:
        with payload_server(probe_bytes, release, args.modules, nonce, 'xhci', script_factory=guest_script) as (base, script, _):
            (output / 'guest-script.sh').write_bytes(script)
            subprocess.run(['sh', '-n'], input=script, check=True)
            with (output / 'qemu.log').open('w') as log:
                process = subprocess.Popen(command, cwd=ROOT, stdout=log, stderr=log)
                deadline = time.monotonic() + 15
                while not socket.exists():
                    if process.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError('owned QEMU startup failed')
                    time.sleep(.1)
                probe = Probe(socket, output)
                wait_log(serial, 'boot complete:', process, timeout=180)
                wait_root(serial, process)
                probe.key('ctrl-alt-f2')
                wait_text(probe, process, 'tty-login', 'login:', timeout=45)
                probe.key('ctrl-u');probe.text('root');probe.key('ret')
                wait_text(probe, process, 'tty-password', 'Password:', timeout=30)
                probe.text('root');probe.key('ret')
                # A shell may print a large MOTD. Verify a nonce-framed serial
                # command instead of searching its off-screen prompt with OCR.
                probe.text('echo HUB_UID:' + nonce + ':$(id -u) >/dev/ttyS0');probe.key('ret')
                wait_marker(serial, 'HUB_UID:' + nonce + ':0', process, timeout=45)
                result['root_uid'] = 0
                probe.text('wget -qO- ' + base + '/u|sh');probe.key('ret')
                wait_marker(serial, 'USB_MOUSE_READY', process)
                usb = probe.command('human-monitor-command', {'command-line': 'info usb'})
                (output / 'usb-before.txt').write_text(usb)
                result['before'] = evidence(usb, serial.read_text(errors='replace'))
                for down in (True, False):
                    probe.command('input-send-event', {'events': [
                        {'type': 'rel', 'data': {'axis': 'x', 'value': 15 if down else 0}},
                        {'type': 'rel', 'data': {'axis': 'y', 'value': 9 if down else 0}},
                        {'type': 'btn', 'data': {'button': 'left', 'down': down}}]})
                    time.sleep(.15)
                wait_marker(serial, 'HUB_DISK_REMOVE_READY', process)
                probe.command('device_del', {'id': 'flash'})
                time.sleep(1)
                probe.command('blockdev-add', {'driver': 'raw', 'node-name': 'stick-replug',
                    'file': {'driver': 'file', 'filename': str(stick)}})
                probe.command('device_add', {'driver': 'usb-storage', 'id': 'flash', 'bus': 'xhci.0',
                                              'drive': 'stick-replug', 'port': '1.3'})
                wait_marker(serial, 'HUB_TREE_REMOVE_READY', process, timeout=120)
                probe.command('device_del', {'id': 'hub0'})
                time.sleep(1)
                probe.command('blockdev-del', {'node-name': 'stick-replug'})
                probe.command('blockdev-add', {'driver': 'raw', 'node-name': 'stick-tree',
                    'file': {'driver': 'file', 'filename': str(stick)}})
                for description in tree_devices(args.controller)[1:]:
                    values = hotplug_arguments(description.replace('drive=stick', 'drive=stick-tree'))
                    probe.command('device_add', values)
                wait_marker(serial, 'HUB_QA_OK:' + nonce, process, timeout=120)
                wait_marker(serial, 'USB_QA_EXIT:' + nonce + ':0', process)
                usb = probe.command('human-monitor-command', {'command-line': 'info usb'})
                (output / 'usb-after.txt').write_text(usb)
                result['after'] = evidence(usb, serial.read_text(errors='replace'),
                                           after='HUB_MODULE_UNLOADED' if args.modules else 'HUB_TREE_REMOVE_READY')
                marker = 'HUB_KEYBOARD_RECONNECTED:' + nonce
                probe.text('echo ' + marker + ' >/dev/ttyS0');probe.key('ret')
                wait_marker(serial, marker, process)
                probe.frame('hub-qa-complete')
                result['runtime_passed'] = True
    except Exception as error:
        result['error'] = str(error)
    finally:
        if probe:
            try:
                probe.frame('last-frame');probe.command('quit')
            except Exception:
                pass
            probe.close()
        if process and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill();process.wait()
        socket.unlink(missing_ok=True)
        result['image_unchanged'] = digest(image) == result['source_sha256']
    if result.get('runtime_passed'):
        target = output / 'host-copied.bin'
        subprocess.run(['mcopy', '-i', str(stick), '::copied.bin', str(target)], check=True)
        result['written_bytes_match'] = target.read_bytes() == PAYLOAD
    result['passed'] = bool(result.get('runtime_passed') and result['image_unchanged'] and result.get('written_bytes_match'))
    (output / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
