#!/usr/bin/env python3
"""Validate packaged USB2 storage/HID in an owned OS snapshot, without PS/2.

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


def guest_script(base, probe_hash, release, modular, nonce):
    """Keep long shell syntax off the QMP keyboard and require exact outcomes."""
    script = f'''#!/bin/sh
exec >/dev/ttyS0 2>&1
set -eu
trap 'result=$?; echo USB_QA_EXIT:$result' EXIT
echo USB_QA_START:{nonce}
test "$(uname -r)" = '{release}'
echo USB_KEYBOARD_OK
wget -qO /tmp/usb-probe {base}/p
echo '{probe_hash}  /tmp/usb-probe' | sha256sum -c -
chmod 700 /tmp/usb-probe
mkdir -p /tmp/usb /tmp/usb-ro /tmp/usb-whole
mount_written() {{
 for candidate in /dev/sd*1; do
  test -b "$candidate" || continue
  mount -t vfat "$candidate" /tmp/usb 2>/dev/null || continue
  if test -f /tmp/usb/copied.bin; then
   echo USB_REPLUG_DEVICE:$candidate
   return 0
  fi
  umount /tmp/usb || return 1
 done
 return 1
}}
mount -t vfat /dev/sda1 /tmp/usb
echo '{PAYLOAD_HASH}  /tmp/usb/payload.bin' | sha256sum -c -
cat /tmp/usb/readme.txt
cp /tmp/usb/payload.bin /tmp/usb/copied.bin
cp /tmp/usb/readme.txt /tmp/usb/copied.txt
sync
echo '{PAYLOAD_HASH}  /tmp/usb/copied.bin' | sha256sum -c -
umount /tmp/usb
echo USB_MBR_RW_OK
/tmp/usb-probe readonly /dev/sdb1 /tmp/usb-ro
echo '{PAYLOAD_HASH}  /tmp/usb-ro/payload.bin' | sha256sum -c -
if touch /tmp/usb-ro/forbidden; then exit 41; fi
umount /tmp/usb-ro
echo USB_GPT_RO_OK
mount -t vfat /dev/sdc /tmp/usb-whole
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
until mount_written; do
 tries=$((tries+1)); test "$tries" -lt 30; sleep 1
done
echo '{PAYLOAD_HASH}  /tmp/usb/copied.bin' | sha256sum -c -
echo USB_REPLUG_OK
'''
    if modular:
        script += '''grep '^ehci ' /proc/modules
if rmmod ehci; then echo USB_MODULE_BUSY_FAILED; exit 42; fi
echo USB_MODULE_BUSY_OK
umount /tmp/usb
rmmod ehci
if grep -q '^ehci ' /proc/modules; then exit 43; fi
echo USB_MODULE_UNLOADED
modprobe ehci
tries=0
until mount_written; do
 tries=$((tries+1)); test "$tries" -lt 30; sleep 1
done
'''
        script += f"echo '{PAYLOAD_HASH}  /tmp/usb/copied.bin' | sha256sum -c -\n"
        script += 'echo USB_MODULE_RELOADED\n'
    script += f'umount /tmp/usb\necho USB_QA_OK:{nonce}\n'
    return script.encode()


@contextmanager
def payload_server(probe, release, modular, nonce):
    """Serve only the two exact nonce URLs on loopback."""
    contents = {'/p': probe}

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            key = self.path.removeprefix('/' + nonce)
            if self.path != '/' + nonce + key or key not in contents:
                self.send_error(404)
                return
            data = contents[key]
            self.send_response(200)
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, *args):
            pass

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    base = f'http://10.0.2.2:{server.server_port}/{nonce}'
    contents['/u'] = guest_script(base, hashlib.sha256(probe).hexdigest(), release, modular, nonce)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        yield base, contents['/u']
    finally:
        server.shutdown()
        server.server_close()
        worker.join(timeout=5)


def wait_marker(serial, marker, process, timeout=90):
    """Reject script failure/panic immediately; require a full fresh output line."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = serial.read_text(errors='replace') if serial.exists() else ''
        if re.search(r'(?im)kernel panic|USB_QA_EXIT:[1-9][0-9]*', text):
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--probe', type=Path, required=True, help='compiled ReliefNT tools/tests/usb_input_probe.c')
    parser.add_argument('--output', type=Path, required=True, help='new owned evidence directory')
    parser.add_argument('--topology', choices=('highspeed', 'companion'), default='highspeed')
    parser.add_argument('--modules', action='store_true', help='also verify busy/unload/reload of ehci.ko')
    args = parser.parse_args()
    image, output = args.image.resolve(), args.output.resolve()
    if not image.is_file() or args.image.is_symlink():
        parser.error('image must be an existing regular build artifact')
    manifest = json.loads(args.manifest.read_bytes())
    release = manifest['release']
    if not re.fullmatch(r'[A-Za-z0-9._+-]{1,31}', release):
        parser.error('invalid kernel release')
    output.mkdir(parents=True, exist_ok=False)
    nonce = uuid.uuid4().hex[:10]
    socket = ROOT.parent / ('.usb2-' + nonce + '.sock')
    if socket.exists() or len(os.fsencode(socket)) >= 104:
        parser.error('owned QMP socket unavailable')
    sticks = [make_stick(output, name, layout) for name, layout in
              (('stick', 'mbr'), ('readonly', 'gpt'), ('whole', 'whole'))]
    result = {'passed': False, 'image_sha256_before': digest(image), 'release': release,
              'manifest_sha256': digest(args.manifest), 'probe_sha256': digest(args.probe),
              'topology': args.topology, 'modules': args.modules}
    serial = output / 'serial.log'
    command = ['qemu-system-x86_64', '-machine', 'q35,i8042=off', '-accel', 'kvm', '-cpu', 'host',
               '-m', '4096', '-smp', '2', '-display', 'none', '-serial', 'file:' + str(serial),
               '-vga', 'vmware', '-netdev', 'user,id=n', '-device', 'e1000,netdev=n',
               '-drive', f'file={image},format=raw,if=none,id=osdisk,snapshot=on',
               '-device', 'nvme,drive=osdisk,serial=usb2qa,bootindex=1',
               '-fw_cfg', 'name=opt/org.seabios/pci64,string=0',
               '-qmp', f'unix:{socket},server=on,wait=off', '-no-reboot', '-no-shutdown']
    if args.topology == 'companion':
        command += ['-device', 'ich9-usb-ehci1,id=ehci,addr=1d.7,multifunction=on']
        for i in range(3):
            command += ['-device', f'ich9-usb-uhci{i+1},addr=1d.{i},multifunction=on,masterbus=ehci.0,firstport={i*2}']
    else:
        command += ['-device', 'usb-ehci,id=ehci']
    for i, (stick, port) in enumerate(zip(sticks, (1, 4, 5))):
        command += ['-drive', f'file={stick},format=raw,if=none,id=stick{i}' + (',readonly=on' if i == 1 else ''),
                    '-device', f'usb-storage,bus=ehci.0,drive=stick{i},id=flash{i},port={port}']
    version = '1' if args.topology == 'companion' else '2'
    command += ['-device', f'usb-kbd,bus=ehci.0,id=kbd,port=2,usb_version={version}',
                '-device', f'usb-mouse,bus=ehci.0,id=mouse,port=3,usb_version={version}']
    result['command'] = command
    probe = None
    process = None
    try:
        with payload_server(args.probe.read_bytes(), release, args.modules, nonce) as (base, script):
            (output / 'guest-script.sh').write_bytes(script)
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
                probe.command('device_add', {'driver': 'usb-storage', 'bus': 'ehci.0',
                    'drive': 'stick-replug', 'id': 'flash-new', 'port': '1'})
                wait_marker(serial, 'USB_QA_OK:' + nonce, process, timeout=120)
                wait_marker(serial, 'USB_QA_EXIT:0', process)
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
        socket.unlink(missing_ok=True)
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
