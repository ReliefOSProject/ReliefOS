#!/usr/bin/env python3
"""Validate packaged USB storage/HID in an owned OS snapshot, without PS/2.

Consumes a native probe from ReliefNT; no private driver implementation lives in
this OS integration test. Only disposable images in a new output directory are
written. A nonce-limited loopback server serves exact test bytes, never files.
"""
import argparse
from contextlib import contextmanager
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import subprocess
import threading
import time
import uuid

from test_installer_accounts_qemu import Probe
from test_installer_window_qemu import wait_log
from test_kernel_modules_qemu import digest, root_login
from test_xorg_vm_qemu import wait_greeter_stage

ROOT = Path(__file__).resolve().parents[1]
PAYLOAD = bytes(range(256)) * 4096
PAYLOAD_HASH = hashlib.sha256(PAYLOAD).hexdigest()
TEXT = 'ReliefOS USB2 中文读写测试\n'.encode()


def run(*command, **kwargs):
    """Fail on every offline fixture/tool error."""
    return subprocess.run(command, check=True, **kwargs)


def make_stick(output, name, layout):
    """Create private FAT32 media, optionally wrapped in GPT or DOS MBR."""
    fat = output / (name + '-fat.raw')
    with fat.open('xb') as stream:
        stream.truncate(64 * 1024 * 1024)
    run('mkfs.vfat', '-F', '32', '-n', 'USBTEST', str(fat), stdout=subprocess.DEVNULL)
    for filename, content in (('payload.bin', PAYLOAD), ('readme.txt', TEXT)):
        source = output / filename
        if not source.exists():
            source.write_bytes(content)
        run('mcopy', '-i', str(fat), str(source), '::' + filename)
    identity = output / (name + '-fixture.txt')
    identity.write_text(name + '\n')
    run('mcopy', '-i', str(fat), str(identity), '::fixture.txt')
    if layout == 'whole':
        return fat
    disk = output / (name + '.raw')
    with disk.open('xb') as stream:
        stream.truncate(96 * 1024 * 1024)
    label, kind = ('dos', 'c') if layout == 'mbr' else ('gpt', 'EBD0A0A2-B9E5-4433-87C0-68B6B72699C7')
    run('sfdisk', str(disk), input=f'label: {label}\nstart=2048,size=131072,type={kind}\n',
        text=True, stdout=subprocess.DEVNULL)
    run('dd', 'if=' + str(fat), 'of=' + str(disk), 'bs=1M', 'seek=1', 'conv=notrunc', 'status=none')
    return disk


def topology_devices(topology, hid_version, sticks):
    """Construct the controller and its five directly attached fixtures."""
    xhci = topology in ('qemu-xhci', 'nec-usb-xhci')
    bus = 'xhci.0' if xhci else 'ehci.0'
    if xhci:
        devices = [f'{topology},id=xhci,p2=8,p3=8']
    elif topology == 'companion':
        devices = ['ich9-usb-ehci1,id=ehci,addr=1d.7,multifunction=on']
        devices += [f'ich9-usb-uhci{i+1},addr=1d.{i},multifunction=on,masterbus=ehci.0,firstport={i*2}'
                    for i in range(3)]
    elif topology == 'highspeed':
        devices = ['usb-ehci,id=ehci']
    else:
        raise ValueError('unknown USB topology')
    version = hid_version or ('1' if topology == 'companion' else '2')
    if version not in ('1', '2') or (not xhci and hid_version is not None):
        raise ValueError('HID USB version override requires an xHCI topology and version 1 or 2')
    for i, (stick, port) in enumerate(zip(sticks, (1, 4, 5))):
        devices += [f'file={stick},format=raw,if=none,id=stick{i}' + (',readonly=on' if i == 1 else ''),
                    f'usb-storage,bus={bus},drive=stick{i},id=flash{i},port={port}']
    devices += [f'usb-kbd,bus={bus},id=kbd,port=2,usb_version={version}',
                f'usb-mouse,bus={bus},id=mouse,port=3,usb_version={version}']
    return bus, devices


def firmware_options(firmware):
    """Use the same read-only OVMF and low NVMe MMIO settings as dual-boot QA."""
    if firmware == 'bios':
        return ['-fw_cfg', 'name=opt/org.seabios/pci64,string=0']
    if firmware != 'uefi':
        raise ValueError('unknown firmware')
    image = Path(os.environ.get('QEMU_FIRMWARE', '/usr/share/edk2/x64/OVMF.4m.fd'))
    if not image.is_file():
        raise FileNotFoundError(f'UEFI firmware unavailable: {image}')
    return ['-bios', str(image), '-fw_cfg', 'name=opt/ovmf/X-PciMmio64Mb,string=0']


def verified_probe(path, identity, required):
    """Freeze the served probe bytes and match the independent identity record."""
    contents = path.read_bytes()
    if identity is None:
        if required:
            raise ValueError('xHCI requires a probe identity record')
        return contents
    try:
        expected = json.loads(identity.read_text())['probe_sha256']
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise ValueError(f'invalid probe identity record: {identity}') from error
    if not isinstance(expected, str) or not re.fullmatch(r'[0-9a-f]{64}', expected):
        raise ValueError('invalid expected probe SHA-256')
    if hashlib.sha256(contents).hexdigest() != expected:
        raise ValueError('probe SHA-256 does not match identity record')
    return contents


def qmp_usb_devices(text):
    """Read per-device addresses, ports and negotiated speeds from HMP info usb."""
    pattern = re.compile(r'^\s*Device (\d+)\.(\d+), Port (\d+), Speed (\d+) Mb/s, '
                         r'Product (.*?), ID: (\S+)\s*$')
    devices = {}
    for line in text.splitlines():
        match = pattern.fullmatch(line)
        if not match:
            continue
        bus, address, port, speed, product, name = match.groups()
        if name in devices:
            raise AssertionError('duplicate QMP USB ID: ' + name)
        devices[name] = {'bus': int(bus), 'address': int(address), 'port': int(port),
                         'speed_mbps': int(speed), 'product': product}
    return devices


def xhci_guest_devices(text):
    """Parse exact xHCI device lines, including negotiated PSI provenance."""
    pattern = re.compile(
        r'\[usb\] xHCI device slot=(\d+) address=(\d+) root_port=(\d+) '
        r'psi=(\d+) speed=(FULL|LOW|HIGH|SUPER|SUPER_PLUS) bps=(\d+) '
        r'psi_raw=([0-9a-fA-F]{8}) psi_source=(explicit|implied) '
        r'vid=([0-9a-fA-F]{4}) pid=([0-9a-fA-F]{4})(?:\s|$)')
    devices = []
    for line in text.splitlines():
        if '[usb] xHCI device ' not in line:
            continue
        match = pattern.search(line)
        if not match:
            raise AssertionError('malformed guest xHCI device log')
        slot, address, root_port, psi, speed, bps, raw, source, vid, pid = match.groups()
        devices.append({'slot': int(slot), 'address': int(address), 'root_port': int(root_port),
                        'psi': int(psi), 'speed': speed, 'bps': int(bps),
                        'psi_raw': raw.lower(), 'psi_source': source,
                        'vid': vid.lower(), 'pid': pid.lower()})
    return devices


def xhci_qtree(text, topology, storage_ids):
    """Require the named controller to own the named devices and backends."""
    lines = text.splitlines()
    controllers = [(i, len(m.group(1))) for i, line in enumerate(lines)
                   if (m := re.fullmatch(r'(\s*)dev: ' + re.escape(topology) + r', id "xhci"\s*', line))]
    if len(controllers) != 1:
        raise AssertionError('missing or duplicate QMP xHCI controller')
    start, indent = controllers[0]
    end = next((i for i in range(start + 1, len(lines))
                if lines[i].strip() and len(lines[i]) - len(lines[i].lstrip()) <= indent), len(lines))
    controller = lines[start:end]
    pci = [re.search(r'class USB controller, addr ([0-9a-fA-F]+):([0-9a-fA-F]+)\.([0-9a-fA-F]+),', line)
           for line in controller]
    pci = [m for m in pci if m]
    buses = [(i, len(m.group(1))) for i, line in enumerate(controller)
             if (m := re.fullmatch(r'(\s*)bus: xhci\.0\s*', line))]
    if len(pci) != 1 or len(buses) != 1:
        raise AssertionError('missing xHCI PCI address or USB bus in QMP tree')
    start, bus_indent = buses[0]
    end = next((i for i in range(start + 1, len(controller))
                if controller[i].strip() and len(controller[i]) - len(controller[i].lstrip()) <= bus_indent), len(controller))
    children = controller[start + 1:end]
    found = {}
    for i, line in enumerate(children):
        match = re.fullmatch(r'(\s*)dev: ([\w-]+), id "([\w-]+)"\s*', line)
        if not match or len(match.group(1)) != bus_indent + 2:
            continue
        name, kind = match.group(3), match.group(2)
        stop = next((j for j in range(i + 1, len(children))
                     if children[j].strip() and len(children[j]) - len(children[j].lstrip()) <= bus_indent + 2), len(children))
        if name in found:
            raise AssertionError('duplicate QMP tree USB ID: ' + name)
        found[name] = (kind, children[i + 1:stop])
    expected = {**{name: ('usb-storage', drive) for name, drive in storage_ids.items()},
                'kbd': ('usb-kbd', None), 'mouse': ('usb-mouse', None)}
    for name, (kind, drive) in expected.items():
        if name not in found or found[name][0] != kind:
            raise AssertionError('wrong xHCI child: ' + name)
        if drive is not None and sum(line.strip() == f'drive = "{drive}"' for line in found[name][1]) != 1:
            raise AssertionError('wrong xHCI storage backend: ' + name)
    return tuple(int(part, 16) for part in pci[0].groups())


def validate_xhci_evidence(usb, qtree, serial, topology, hid_version, *, replug=False,
                           replug_serial=None, reload=False):
    """Correlate each QEMU fixture with its own xHCI address and root-port speed."""
    names = ('flash-new' if replug else 'flash0', 'flash1', 'flash2', 'kbd', 'mouse')
    drives = {names[0]: 'stick-replug' if replug else 'stick0', names[1]: 'stick1', names[2]: 'stick2'}
    pci = xhci_qtree(qtree, topology, drives)
    epoch = serial
    if reload:
        if not replug:
            raise AssertionError('module reload requires the replug topology')
        lines = serial.splitlines(keepends=True)
        boundaries = [i for i, line in enumerate(lines) if line.strip() == 'USB_MODULE_UNLOADED']
        if len(boundaries) != 1:
            raise AssertionError('missing or duplicate module unload boundary')
        epoch = ''.join(lines[boundaries[0] + 1:])
    ready = re.findall(r'\[usb\] xHCI pci=(\d+):(\d+)\.(\d+) ports=(\d+)\b', epoch)
    if not any(tuple(map(int, row[:3])) == pci and int(row[3]) >= 16 for row in ready):
        raise AssertionError('guest xHCI controller identity or root-port count missing')
    qemu = qmp_usb_devices(usb)
    guest = xhci_guest_devices(epoch)
    if replug and not reload:
        if replug_serial is None:
            raise AssertionError('missing host post-attach serial epoch')
        replug_guest = xhci_guest_devices(replug_serial)
    expected = [(1, 1, 5000, 'SUPER', 5000000000, '46f4', '0001', 'QEMU USB MSD'),
                (4, 4, 5000, 'SUPER', 5000000000, '46f4', '0001', 'QEMU USB MSD'),
                (5, 5, 5000, 'SUPER', 5000000000, '46f4', '0001', 'QEMU USB MSD')]
    hid_speed, hid_bps, hid_mbps = ('FULL', 12000000, 12) if hid_version == '1' else ('HIGH', 480000000, 480)
    expected += [(2, 10, hid_mbps, hid_speed, hid_bps, '0627', '0001', 'QEMU USB Keyboard'),
                 (3, 11, hid_mbps, hid_speed, hid_bps, '0627', '0001', 'QEMU USB Mouse')]
    result = {}
    for name, (port, root, mbps, speed, bps, vid, pid, product) in zip(names, expected):
        item = qemu.get(name)
        if not item or (item['port'], item['speed_mbps'], item['product']) != (port, mbps, product):
            raise AssertionError('QMP port/speed/product mismatch: ' + name)
        candidates = replug_guest if replug and not reload and name == 'flash-new' else guest
        matches = [device for device in candidates if device['root_port'] == root]
        if not matches or (matches[-1]['address'], matches[-1]['speed'], matches[-1]['bps'],
                           matches[-1]['vid'], matches[-1]['pid']) != (item['address'], speed, bps, vid, pid):
            raise AssertionError('guest xHCI identity/speed mismatch: ' + name)
        result[name] = {'qemu': item, 'guest': matches[-1]}
    if len({item['qemu']['bus'] for item in result.values()}) != 1:
        raise AssertionError('QMP USB devices are on different buses')
    if len({item['qemu']['address'] for item in result.values()}) != len(result):
        raise AssertionError('QMP USB device addresses are not unique')
    return result


def validate_guest_output(serial, nonce, release, probe_hash, modular, firmware):
    """Require the current run's exact success, identity and lifecycle markers."""
    lines = [line.strip() for line in serial.splitlines()]
    boots = re.findall(r'\[boot\] firmware=(legacy|UEFI)\b', serial)
    if boots != [('legacy' if firmware == 'bios' else 'UEFI')]:
        raise AssertionError('guest firmware identity mismatch')
    replug = [line.split(':', 1)[1] for line in lines if line.startswith('USB_REPLUG_DEVICE:')]
    if len(replug) != 1 or not re.fullmatch(r'/dev/sd[a-z]+1', replug[0]) or replug[0] == '/dev/sda1':
        raise AssertionError('replug did not yield a fresh device name')
    module_devices = [line.split(':', 1)[1] for line in lines if line.startswith('USB_MODULE_DEVICE:')]
    if modular and (len(module_devices) != 1 or not re.fullmatch(r'/dev/sd[a-z]+1', module_devices[0])):
        raise AssertionError('missing or duplicate module reload device')
    if not modular and module_devices:
        raise AssertionError('unexpected module reload device')
    markers = [f'USB_QA_START:{nonce}', f'USB_KERNEL_RELEASE:{release}', 'USB_KEYBOARD_OK',
               f'USB_PROBE_SHA256:{probe_hash}', 'USB_FIXTURE:stick:/dev/sda1', 'USB_MBR_RW_OK',
               'USB_RAW_WRITE_EROFS', 'USB_RO_MOUNT_REMOUNT_OK', 'USB_FIXTURE:readonly:/dev/sdb1',
               'USB_GPT_RO_OK', 'USB_FIXTURE:whole:/dev/sdc', 'USB_WHOLE_OK',
               'USB_MOUSE_READY', 'USB_MOUSE_OK', 'USB_REMOVE_READY', 'USB_OLD_FD_READY',
               'USB_OLD_FD_ENODEV', 'USB_REPLUG_DEVICE:' + replug[0], 'USB_REPLUG_OK']
    if modular:
        markers += ['USB_MODULE_BUSY_OK', 'USB_MODULE_UNLOADED',
                    'USB_MODULE_DEVICE:' + module_devices[0], 'USB_MODULE_RELOADED']
    markers += [f'USB_QA_OK:{nonce}', f'USB_QA_EXIT:{nonce}:0']
    if any(lines.count(marker) != 1 for marker in markers):
        raise AssertionError('missing or duplicate guest success/identity marker')
    for prefix, count in (('USB_QA_START:', 1), ('USB_QA_OK:', 1), ('USB_QA_EXIT:', 1), ('USB_FIXTURE:', 3)):
        if sum(line.startswith(prefix) for line in lines) != count:
            raise AssertionError('unexpected or duplicate guest identity marker')
    positions = [lines.index(marker) for marker in markers]
    if any(first >= second for first, second in zip(positions, positions[1:])):
        raise AssertionError('guest lifecycle order invalid')
    return {'fixtures': {'stick': '/dev/sda1', 'readonly': '/dev/sdb1', 'whole': '/dev/sdc'},
            'replug': replug[0]}


def guest_script(base, probe_hash, release, modular, nonce, module='ehci'):
    """Keep long shell syntax off the QMP keyboard and require exact outcomes."""
    script = f'''#!/bin/sh
exec >/dev/ttyS0 2>&1
set -eu
trap 'result=$?; echo USB_QA_EXIT:{nonce}:$result' EXIT
echo USB_QA_START:{nonce}
test "$(uname -r)" = '{release}'
echo USB_KERNEL_RELEASE:{release}
echo USB_KEYBOARD_OK
wget -qO /tmp/usb-probe {base}/p
echo '{probe_hash}  /tmp/usb-probe' | sha256sum -c -
echo USB_PROBE_SHA256:{probe_hash}
chmod 700 /tmp/usb-probe
mkdir -p /tmp/usb /tmp/usb-ro /tmp/usb-whole
mount_written() {{
 for candidate in /dev/sd*1; do
  test -b "$candidate" || continue
  test "$candidate" != /dev/sda1 || continue
  mount -t vfat "$candidate" /tmp/usb 2>/dev/null || continue
  if test -f /tmp/usb/copied.bin && test "$(cat /tmp/usb/fixture.txt)" = stick; then
   echo USB_${{1}}_DEVICE:$candidate
   return 0
  fi
  umount /tmp/usb || return 1
 done
 return 1
}}
mount -t vfat /dev/sda1 /tmp/usb
test "$(cat /tmp/usb/fixture.txt)" = stick
echo USB_FIXTURE:stick:/dev/sda1
echo '{PAYLOAD_HASH}  /tmp/usb/payload.bin' | sha256sum -c -
cat /tmp/usb/readme.txt
cp /tmp/usb/payload.bin /tmp/usb/copied.bin
cp /tmp/usb/readme.txt /tmp/usb/copied.txt
sync
echo '{PAYLOAD_HASH}  /tmp/usb/copied.bin' | sha256sum -c -
umount /tmp/usb
echo USB_MBR_RW_OK
/tmp/usb-probe readonly /dev/sdb1 /tmp/usb-ro
test "$(cat /tmp/usb-ro/fixture.txt)" = readonly
echo USB_FIXTURE:readonly:/dev/sdb1
echo '{PAYLOAD_HASH}  /tmp/usb-ro/payload.bin' | sha256sum -c -
if touch /tmp/usb-ro/forbidden; then exit 41; fi
umount /tmp/usb-ro
echo USB_GPT_RO_OK
mount -t vfat /dev/sdc /tmp/usb-whole
test "$(cat /tmp/usb-whole/fixture.txt)" = whole
echo USB_FIXTURE:whole:/dev/sdc
echo '{PAYLOAD_HASH}  /tmp/usb-whole/payload.bin' | sha256sum -c -
umount /tmp/usb-whole
echo USB_WHOLE_OK
xdm_pid=$(cat /run/xdm.pid)
case "$xdm_pid" in ''|*[!0-9]*) exit 44;; esac
kill "$xdm_pid"
tries=0
while kill -0 "$xdm_pid" 2>/dev/null; do
 tries=$((tries+1)); test "$tries" -lt 15; sleep 1
done
/tmp/usb-probe mouse
/tmp/usb-probe block /dev/sda >/tmp/old-usb.log 2>&1 &
holder=$!
tries=0
until grep -q USB_OLD_FD_READY /tmp/old-usb.log; do
 tries=$((tries+1)); test "$tries" -lt 8; sleep 1
done
echo USB_REMOVE_READY
wait "$holder"
cat /tmp/old-usb.log
grep -q USB_OLD_FD_ENODEV /tmp/old-usb.log
tries=0
until mount_written REPLUG; do
 tries=$((tries+1)); test "$tries" -lt 30; sleep 1
done
echo '{PAYLOAD_HASH}  /tmp/usb/copied.bin' | sha256sum -c -
echo USB_REPLUG_OK
wget -qO /dev/null {base}/gate
'''
    if modular:
        script += f'''grep '^{module} ' /proc/modules
if rmmod {module}; then echo USB_MODULE_BUSY_FAILED; exit 42; fi
echo USB_MODULE_BUSY_OK
umount /tmp/usb
rmmod {module}
if grep -q '^{module} ' /proc/modules; then exit 43; fi
echo USB_MODULE_UNLOADED
modprobe {module}
tries=0
until mount_written MODULE; do
 tries=$((tries+1)); test "$tries" -lt 30; sleep 1
done
'''
        script += f"echo '{PAYLOAD_HASH}  /tmp/usb/copied.bin' | sha256sum -c -\n"
        script += 'echo USB_MODULE_RELOADED\n'
    script += f'umount /tmp/usb\necho USB_QA_OK:{nonce}\n'
    return script.encode()


@contextmanager
def payload_server(probe, release, modular, nonce, module='ehci'):
    """Serve exact nonce URLs and hold the guest until replug evidence is read."""
    contents = {'/p': probe}
    gate = threading.Event()

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            key = self.path.removeprefix('/' + nonce)
            if self.path != '/' + nonce + key or (key not in contents and key != '/gate'):
                self.send_error(404)
                return
            if key == '/gate':
                if not gate.wait(timeout=180):
                    self.send_error(504)
                    return
                data = b'continue\n'
            else:
                data = contents[key]
            self.send_response(200)
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, *args):
            pass

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    base = f'http://10.0.2.2:{server.server_port}/{nonce}'
    contents['/u'] = guest_script(base, hashlib.sha256(probe).hexdigest(), release, modular, nonce, module)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        yield base, contents['/u'], gate.set
    finally:
        gate.set()
        server.shutdown()
        server.server_close()
        worker.join(timeout=5)


def attach_usb_device(probe, serial, bus):
    """Attach while the guest is stopped, then return the post-attach byte offset."""
    probe.command('stop')
    try:
        probe.command('device_add', {'driver': 'usb-storage', 'bus': bus,
                                    'drive': 'stick-replug', 'id': 'flash-new', 'port': '1'})
        return serial.stat().st_size
    finally:
        probe.command('cont')


def guest_failed(text):
    """Read the complete exit record; a numeric nonce is not an exit status."""
    return bool(re.search(r'(?i)kernel panic', text) or
                re.search(r'(?m)^USB_QA_EXIT:(?:[0-9a-f]+:)?[1-9][0-9]*\r?\n', text))


def wait_marker(serial, marker, process, timeout=90):
    """Reject script failure/panic immediately; require a full fresh output line."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = serial.read_text(errors='replace') if serial.exists() else ''
        if guest_failed(text):
            raise AssertionError('guest panic or USB QA failure; see ' + str(serial))
        if any(line.strip() == marker for line in text.splitlines()):
            return
        if process.poll() is not None:
            raise RuntimeError('owned QEMU exited before ' + marker)
        time.sleep(.1)
    raise AssertionError('missing guest output: ' + marker)


def wait_root(serial, process):
    """Stop promptly on a real initramfs failure instead of waiting for XDM."""
    deadline = time.monotonic() + 180
    while time.monotonic() < deadline:
        text = serial.read_text(errors='replace') if serial.exists() else ''
        if '[initramfs] ERROR:' in text or re.search(r'(?im)kernel panic', text):
            raise AssertionError('guest root startup failed; see ' + str(serial))
        if '[initramfs] root=' in text:
            return
        if process.poll() is not None:
            raise RuntimeError('owned QEMU exited before switch_root')
        time.sleep(.2)
    raise AssertionError('guest did not reach switch_root')


def wait_xhci_reload(probe, serial, process, output, topology, hid_version, *, timeout=30,
                     monotonic=time.monotonic, sleep=time.sleep):
    """Wait for all fresh reload events to agree with the current QMP topology."""
    deadline = monotonic() + timeout
    last_error = 'no evidence observed'

    def enumeration_lines(value):
        return [line for line in value.splitlines()
                if '[usb] xHCI device ' in line or '[usb] xHCI pci=' in line or
                line.strip() == 'USB_MODULE_UNLOADED']

    while True:
        text = serial.read_text(errors='replace')
        before = text
        if guest_failed(text):
            raise AssertionError('guest panic or USB QA failure; see ' + str(serial))
        if process.poll() is not None:
            raise RuntimeError('owned QEMU exited while waiting for xHCI reload')
        usb = probe.command('human-monitor-command', {'command-line': 'info usb'})
        (output / 'usb-after.txt').write_text(usb)
        qtree = probe.command('human-monitor-command', {'command-line': 'info qtree'})
        (output / 'qtree-after.txt').write_text(qtree)
        # Events may arrive while either QMP query is in flight.
        text = serial.read_text(errors='replace')
        if guest_failed(text):
            raise AssertionError('guest panic or USB QA failure; see ' + str(serial))
        if process.poll() is not None:
            raise RuntimeError('owned QEMU exited while waiting for xHCI reload')
        if enumeration_lines(before) != enumeration_lines(text):
            last_error = 'guest xHCI enumeration changed during QMP capture'
            if monotonic() < deadline:
                continue
            raise AssertionError('xHCI reload evidence timed out; last evidence: ' + last_error +
                                 '; see serial.log, usb-after.txt and qtree-after.txt')
        try:
            evidence = validate_xhci_evidence(usb, qtree, text, topology, hid_version,
                                              replug=True, reload=True)
        except AssertionError as error:
            last_error = str(error)
        else:
            if monotonic() <= deadline:
                return evidence, text
            last_error = 'complete evidence arrived after the deadline'
        remaining = deadline - monotonic()
        if remaining <= 0:
            raise AssertionError('xHCI reload evidence timed out; last evidence: ' + last_error +
                                 '; see serial.log, usb-after.txt and qtree-after.txt')
        sleep(min(.1, remaining))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--probe', type=Path, required=True, help='compiled ReliefNT tools/tests/usb_input_probe.c')
    parser.add_argument('--probe-identity', type=Path, help='JSON record containing expected probe_sha256 (required for xHCI; defaults beside probe)')
    parser.add_argument('--output', type=Path, required=True, help='new owned evidence directory')
    parser.add_argument('--topology', choices=('highspeed', 'companion', 'qemu-xhci', 'nec-usb-xhci'), default='highspeed')
    parser.add_argument('--hid-usb-version', choices=('1', '2'), help='xHCI boot HID USB version (default 2)')
    parser.add_argument('--firmware', choices=('bios', 'uefi'), default='bios')
    parser.add_argument('--modules', action='store_true', help='also verify busy/unload/reload of the selected HCD module')
    args = parser.parse_args()
    xhci = args.topology in ('qemu-xhci', 'nec-usb-xhci')
    if not xhci and args.hid_usb_version is not None:
        parser.error('--hid-usb-version is available only for xHCI topologies')
    image, output = args.image.resolve(), args.output.resolve()
    if not image.is_file() or args.image.is_symlink():
        parser.error('image must be an existing regular build artifact')
    manifest = json.loads(args.manifest.read_bytes())
    release = manifest['release']
    if not re.fullmatch(r'[A-Za-z0-9._+-]{1,31}', release):
        parser.error('invalid kernel release')
    identity = args.probe_identity or (args.probe.parent / 'probe-identity.json' if xhci else None)
    try:
        probe_bytes = verified_probe(args.probe, identity, xhci)
        fw_options = firmware_options(args.firmware)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    output.mkdir(parents=True, exist_ok=False)
    nonce = uuid.uuid4().hex[:10]
    socket_dir = ROOT.parent / ('.usbqa-' + nonce)
    socket = socket_dir / 'qmp.sock'
    if socket_dir.exists() or len(os.fsencode(socket)) >= 104:
        parser.error('owned QMP socket unavailable')
    sticks = [make_stick(output, name, layout) for name, layout in
              (('stick', 'mbr'), ('readonly', 'gpt'), ('whole', 'whole'))]
    probe_hash = hashlib.sha256(probe_bytes).hexdigest()
    version = args.hid_usb_version or ('1' if args.topology == 'companion' else '2')
    module = 'xhci' if xhci else 'ehci'
    result = {'passed': False, 'image_sha256_before': digest(image), 'release': release,
              'manifest_sha256': digest(args.manifest), 'probe_sha256': probe_hash,
              'probe_identity': str(identity) if identity else None, 'nonce': nonce,
              'topology': args.topology, 'firmware': args.firmware, 'hid_usb_version': version,
              'modules': args.modules, 'module': module}
    serial = output / 'serial.log'
    command = ['qemu-system-x86_64', '-machine', 'q35,i8042=off', '-accel', 'kvm', '-cpu', 'host',
               '-m', '4096', '-smp', '2', '-display', 'none', '-serial', 'file:' + str(serial),
               '-vga', 'vmware', '-netdev', 'user,id=n', '-device', 'e1000,netdev=n',
               '-drive', f'file={image},format=raw,if=none,id=osdisk,snapshot=on',
               '-device', 'nvme,drive=osdisk,serial=usb2qa,bootindex=1',
               '-qmp', f'unix:{socket},server=on,wait=off', '-no-reboot', '-no-shutdown']
    command += fw_options
    bus, devices = topology_devices(args.topology, args.hid_usb_version, sticks)
    for device in devices:
        command += ['-drive' if device.startswith('file=') else '-device', device]
    result['command'] = command
    probe = None
    process = None
    owned_socket_dir = False
    try:
        socket_dir.mkdir(mode=0o700)
        owned_socket_dir = True
        with payload_server(probe_bytes, release, args.modules, nonce, module) as (base, script, release_gate):
            (output / 'guest-script.sh').write_bytes(script)
            run('sh', '-n', input=script)
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
                wait_greeter_stage(probe, process, 'login', 'greeter', timeout=180)
                probe.key('ctrl-alt-f2')
                root_login(probe, process, result)
                probe.text('wget -qO- ' + base + '/u|sh')
                probe.key('ret')
                wait_marker(serial, 'USB_MOUSE_READY', process)
                if xhci:
                    usb = probe.command('human-monitor-command', {'command-line': 'info usb'})
                    qtree = probe.command('human-monitor-command', {'command-line': 'info qtree'})
                    (output / 'usb-before.txt').write_text(usb)
                    (output / 'qtree-before.txt').write_text(qtree)
                    result['xhci_before'] = validate_xhci_evidence(
                        usb, qtree, serial.read_text(errors='replace'), args.topology, version)
                result['mouse_routes'] = probe.command('human-monitor-command', {'command-line': 'info mice'})
                (output / 'mouse-routes.txt').write_text(result['mouse_routes'])
                probe.command('input-send-event', {'events': [
                    {'type': 'rel', 'data': {'axis': 'x', 'value': 15}},
                    {'type': 'rel', 'data': {'axis': 'y', 'value': 9}},
                    {'type': 'btn', 'data': {'button': 'left', 'down': True}}]})
                time.sleep(.15)
                probe.command('input-send-event', {'events': [
                    {'type': 'btn', 'data': {'button': 'left', 'down': False}}]})
                wait_marker(serial, 'USB_REMOVE_READY', process)
                probe.command('device_del', {'id': 'flash0'})
                time.sleep(1)
                # -drive backends are auto-deleted with this USB device.
                # Reinsert the same owned bytes through a fresh block node.
                probe.command('blockdev-add', {'driver': 'raw', 'node-name': 'stick-replug',
                    'file': {'driver': 'file', 'filename': str(sticks[0])}})
                replug_offset = attach_usb_device(probe, serial, bus)
                result['replug_serial_offset'] = replug_offset
                wait_marker(serial, 'USB_REPLUG_OK', process, timeout=120)
                serial_bytes = serial.read_bytes()
                if replug_offset > len(serial_bytes):
                    raise AssertionError('guest serial log was truncated after attach')
                replug_serial = serial_bytes[replug_offset:].decode(errors='replace')
                if xhci:
                    usb = probe.command('human-monitor-command', {'command-line': 'info usb'})
                    qtree = probe.command('human-monitor-command', {'command-line': 'info qtree'})
                    (output / 'usb-replug.txt').write_text(usb)
                    (output / 'qtree-replug.txt').write_text(qtree)
                    result['xhci_replug'] = validate_xhci_evidence(
                        usb, qtree, serial_bytes.decode(errors='replace'), args.topology, version,
                        replug=True, replug_serial=replug_serial)
                release_gate()
                wait_marker(serial, 'USB_QA_OK:' + nonce, process, timeout=120)
                wait_marker(serial, 'USB_QA_EXIT:' + nonce + ':0', process)
                serial_text = serial.read_text(errors='replace')
                result['guest'] = validate_guest_output(serial_text, nonce, release, probe_hash, args.modules, args.firmware)
                if xhci:
                    if args.modules:
                        result['xhci_after'], serial_text = wait_xhci_reload(
                            probe, serial, process, output, args.topology, version)
                        result['guest'] = validate_guest_output(serial_text, nonce, release, probe_hash,
                                                                args.modules, args.firmware)
                    else:
                        usb = probe.command('human-monitor-command', {'command-line': 'info usb'})
                        qtree = probe.command('human-monitor-command', {'command-line': 'info qtree'})
                        (output / 'usb-after.txt').write_text(usb)
                        (output / 'qtree-after.txt').write_text(qtree)
                        result['xhci_after'] = validate_xhci_evidence(
                            usb, qtree, serial_text, args.topology, version, replug=True,
                            replug_serial=serial.read_bytes()[replug_offset:].decode(errors='replace'))
                probe.frame('usb-qa-complete')
                result['runtime_passed'] = True
    except Exception as error:
        result['error'] = str(error)
    finally:
        if probe is not None:
            try:
                probe.frame('last-frame')
                probe.command('quit')
            except Exception:
                pass
            probe.close()
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        if owned_socket_dir:
            socket.unlink(missing_ok=True)
            socket_dir.rmdir()
        result['image_sha256_after'] = digest(image)
        result['immutable_image'] = result['image_sha256_after'] == result['image_sha256_before']
    if result.get('runtime_passed'):
        try:
            for name, expected in (('copied.bin', PAYLOAD), ('copied.txt', TEXT)):
                target = output / ('host-' + name)
                run('mcopy', '-i', str(sticks[0]) + '@@1048576', '::' + name, str(target))
                if target.read_bytes() != expected:
                    raise AssertionError('offline USB write verification: ' + name)
            result['offline_written_bytes_match'] = True
        except Exception as error:
            result['error'] = str(error)
    result['passed'] = bool(result.get('runtime_passed') and result['immutable_image'] and
                            result.get('offline_written_bytes_match'))
    (output / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
