#!/usr/bin/env python3
"""Verify mail TLS and signed CA recovery in a private QEMU desktop.

Only the private disk trusts the generated fixture CA/package key. No mail is
sent, no real account credentials are used, and the input image is hash checked.
"""
from pathlib import Path
import argparse
import code
import functools
import http.server
import json
import os
import re
import shutil
import socket
import ssl
import subprocess
import sys
import threading
import time
import traceback
import hashlib

import test_fltk_apps_qemu as fltk
import test_xorg_vm_qemu as xorg
from test_reliefmath_qemu import Session, dialog_origin, wait_focus
from test_reliefcapture_qemu import desktop_settled
from test_reliefdiagram_qemu import explicit_keys
from qmp_terminal_smoke import text_keys
from apk_distribution import make_package

sys.path.insert(0, str(Path(__file__).resolve().parent / 'tests'))
import reliefmail_protocol_server as protocol

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'out/xorg-vmware-debug'
CA_VERSION = '99999999.0-r0'  # Higher than date-based upstream versions, e.g. 20260909-r0.


class RepositoryHandler(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        self.server.request_paths.append(self.path)
        super().do_GET()

    def log_message(self, *_args):
        pass


def run(args, log=None):
    return subprocess.run(list(map(str, args)), cwd=ROOT, check=True,
                          stdout=log if log else subprocess.PIPE,
                          stderr=log if log else subprocess.PIPE)


def build_probes(output):
    probe = fltk.target_compile_probe(output, OUT / 'sysroot/musl', 'clang++', 'ld.lld')
    template = json.loads((output / 'probe-build.command.json').read_text())
    objects = []
    with (output / 'tls-probe-build.log').open('w') as log:
        for name, source in [('tls-probe', ROOT / 'tools/tests/reliefmail_tls_probe.cpp'),
                             ('transport', ROOT / 'userland/apps/reliefmail/transport.cpp'),
                             ('model', ROOT / 'userland/apps/reliefmail/model.cpp')]:
            command = list(template['compile'])
            command[-3], command[-1] = str(source), str(output / (name + '.o'))
            command[1:1] = ['-I' + str(ROOT / 'userland/apps/reliefmail'),
                             '-I' + str(ROOT / 'userland/runtime/include'),
                             '-I' + str(ROOT / 'third_party/mbedtls/include'),
                             '-DMBEDTLS_CONFIG_FILE="reliefos_mbedtls_config.h"']
            run(command, log)
            objects.append(str(output / (name + '.o')))
        command = list(template['link'])
        index = command.index(str(output / 'fltk_ui_probe.o'))
        command[index:index + 1] = objects
        command[command.index(str(probe))] = str(output / 'tls-probe.elf')
        command.insert(command.index('-lc'), str(OUT / 'system/lib/libreliefos.so.2'))
        run(command, log)
    return probe


def fixtures(output):
    certs = output / 'certificates'
    with (output / 'fixture-build.log').open('w') as log:
        run([sys.executable, ROOT / 'tools/tests/reliefmail_protocol_server.py', '--prepare-certs', certs], log)
        extension = certs / 'server.ext'
        extension.write_text('basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\n'
                             'extendedKeyUsage=serverAuth\nsubjectAltName=DNS:localhost,DNS:mail.reliefmail.test,IP:10.0.2.2\n')
        run(['openssl', 'req', '-newkey', 'rsa:2048', '-nodes', '-subj', '/CN=localhost',
             '-keyout', certs / 'valid.key', '-out', certs / 'vm.csr'], log)
        run(['openssl', 'x509', '-req', '-in', certs / 'vm.csr', '-CA', certs / 'ca.crt',
             '-CAkey', certs / 'ca.key', '-CAcreateserial', '-days', '365', '-extfile', extension,
             '-out', certs / 'valid.crt'], log)
        key = output / 'package-test-key.pem'
        run(['openssl', 'genpkey', '-algorithm', 'RSA', '-pkeyopt', 'rsa_keygen_bits:2048', '-out', key], log)
        key.chmod(0o600)
        public = output / 'reliefmail-ca-test.rsa.pub'
        run(['openssl', 'pkey', '-in', key, '-pubout', '-out', public], log)
        bundle = output / 'fixture-bundle.pem'
        bundle.write_bytes((ROOT / 'system/certs/cacert.pem').read_bytes() + b'\n' + (certs / 'ca.crt').read_bytes())
        # The recovery CA verifies HTTPS; only the signed update supplies the
        # distinct issuer used by IMAP. A skipped package cannot pass login.
        updated_bundle = output / 'updated-bundle.pem'
        updated_bundle.write_bytes(bundle.read_bytes() + b'\n' + (certs / 'untrusted-ca.crt').read_bytes())
        imap_certs = output / 'imap-certificates'
        imap_certs.mkdir()
        run(['openssl', 'x509', '-req', '-in', certs / 'vm.csr', '-CA', certs / 'untrusted-ca.crt',
             '-CAkey', certs / 'untrusted-ca.key', '-CAcreateserial', '-days', '365', '-extfile', extension,
             '-out', imap_certs / 'valid.crt'], log)
        (imap_certs / 'valid.key').write_bytes((certs / 'valid.key').read_bytes())
        repository = output / 'repository'
        repository.mkdir()
        payload = output / 'ca-payload/etc/ssl/certs'
        payload.mkdir(parents=True)
        (payload / 'ca-certificates.crt').write_bytes(updated_bundle.read_bytes())
        package = repository / f'ca-certificates-bundle-{CA_VERSION}.apk'
        apk = OUT / 'upstream/apk/apk.static'
        make_package(apk, key, output / 'ca-payload', package, 'ca-certificates-bundle', CA_VERSION, [])
        # Retain the actual updater, Mozilla sources and hash hook from the
        # production CA package. Add fixture roots as source files and run its
        # real regeneration trigger: a bundle-only update must not pass here.
        managed = OUT / 'rootfs/managed'
        sources = output / 'ca-source-payload'
        installed = (managed / 'lib/apk/db/installed').read_text()
        block = next(block for block in installed.split('\n\n')
                     if '\nP:ca-certificates\n' in '\n' + block + '\n')
        folder = ''
        for entry in block.splitlines():
            if entry.startswith('F:'): folder = entry[2:]
            elif entry.startswith('R:'):
                relative = Path(folder) / entry[2:]
                target = sources / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(managed / relative, target, follow_symlinks=False)
        local = sources / 'usr/local/share/ca-certificates'
        local.mkdir(parents=True, exist_ok=True)
        shutil.copy2(certs / 'ca.crt', local / 'reliefmail-https-test.crt')
        shutil.copy2(certs / 'untrusted-ca.crt', local / 'reliefmail-imap-test.crt')
        trigger = output / 'ca-source-trigger'
        trigger.write_text('#!/bin/sh\n/usr/sbin/update-ca-certificates\n')
        source_package = repository / f'ca-certificates-{CA_VERSION}.apk'
        make_package(apk, key, sources, source_package, 'ca-certificates', CA_VERSION,
                     ['/bin/sh', 'so:libc.musl-x86_64.so.1', 'so:libcrypto.so.3'],
                     provides=[f'cmd:c_rehash={CA_VERSION}', f'cmd:update-ca-certificates={CA_VERSION}'],
                     scripts=[('trigger', trigger)],
                     triggers=['/usr/share/ca-certificates', '/usr/local/share/ca-certificates',
                               '/etc/ssl/certs', '/etc/ca-certificates/update.d'])
        keys = output / 'keys'
        keys.mkdir()
        (keys / public.name).write_bytes(public.read_bytes())
        run([apk, 'mkndx', '--keys-dir', keys, '--sign-key', key,
             '--output', repository / 'packages.adb', package, source_package], log)
    os.environ['RELIEFMAIL_FIXTURE_CERT_DIR'] = str(imap_certs)
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen(8)
    record = protocol._new_record('imap-login')
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0),
        functools.partial(RepositoryHandler, directory=str(repository)))
    server.request_paths = []
    tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    tls.load_cert_chain(certs / 'valid.crt', certs / 'valid.key')
    server.socket = tls.wrap_socket(server.socket, server_side=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return listener, record, server, bundle, public


def prepare(image, output, bundle, public, server_port):
    probe = build_probes(output)
    repositories = output / 'repositories'
    repositories.write_text(f'https://mail.reliefmail.test:{server_port}/packages.adb\n'
                            f'@alpine https://mail.reliefmail.test:{server_port}/packages.adb\n')
    hosts = output / 'hosts'
    hosts.write_bytes((ROOT / 'system/rootfs/etc/hosts').read_bytes() + b'\n10.0.2.2 mail.reliefmail.test\n')
    broken = output / 'broken-ca'
    broken.write_text('invalid CA fixture: repair must replace this file\n')
    runner = output / 'mail-runner'
    runner.write_text('''#!/bin/sh
export LC_ALL=zh_CN.UTF-8 LANG=zh_CN.UTF-8
export XDG_CONFIG_HOME=/home/test/.config/mail-ca-fixture
export XDG_DATA_HOME=/home/test/.local/share/mail-ca-fixture
export XDG_CACHE_HOME=/home/test/.cache/mail-ca-fixture
/usr/lib/reliefos/apps/reliefmail/reliefmail.elf >/dev/ttyS0 2>&1 &
pid=$!
echo "MAIL_CA_START pid=$pid" >/dev/ttyS0
/usr/lib/reliefos/tests/fltk-ui-probe --watch-window-focus "$pid" >/dev/ttyS0 2>&1 &
watcher=$!
wait "$pid"
status=$?
kill "$watcher" 2>/dev/null
echo "MAIL_CA_EXIT status=$status" >/dev/ttyS0
''')
    payload = [(OUT / f'userland/{app}.elf', f'/usr/lib/reliefos/apps/{app}/{app}.elf', 0o100755)
               for app in ['reliefmail', 'reliefapk']]
    payload += [(OUT / 'system/lib/libreliefos.so.2', '/usr/lib/reliefos/libreliefos.so.2', 0o100755),
                (OUT / 'generated/nls/zh_CN/LC_MESSAGES/leonos.mo', fltk.LOCALE_GUEST, 0o100644),
                (bundle, '/usr/share/reliefos/certs/ca-certificates.crt', 0o100644),
                (broken, '/etc/ssl/certs/ca-certificates.crt', 0o100644),
                (public, '/etc/apk/keys/' + public.name, 0o100644),
                (repositories, '/etc/apk/repositories', 0o100644),
                (hosts, '/etc/hosts', 0o100644),
                (ROOT / 'system/rootfs/etc/apk/protected_paths.d/reliefos-ca-bundle.list',
                 '/etc/apk/protected_paths.d/reliefos-ca-bundle.list', 0o100644),
                (runner, '/usr/bin/mailcatest', 0o100755),
                (probe, fltk.PROBE_GUEST, 0o100755),
                (output / 'tls-probe.elf', '/usr/lib/reliefos/tests/mail-tls-probe', 0o100755)]
    metadata = fltk.prepare_disk(image, OUT / 'generated/system/kernel.sys', output, payload)
    (output / 'prepare.json').write_text(json.dumps(metadata, indent=2) + '\n')
    return metadata


def command(s, command, marker, timeout=60):
    offset = len(s.serial.read_text(errors='replace'))
    s.probe.text(command)
    s.probe.key('ret')
    xorg.wait_serial_after(s.serial, marker, s.process, offset, timeout=timeout)


def keyboard(s):
    explicit_keys(s)
    punctuation = {'@': 'shift-2', '>': 'shift-dot', '<': 'shift-comma',
                   ';': 'semicolon', '"': 'shift-apostrophe', "'": 'apostrophe',
                   '(': 'shift-9', ')': 'shift-0', '$': 'shift-4', '?': 'shift-slash'}
    def text(value):
        for ch in value:
            s.probe.key(punctuation.get(ch, text_keys(ch)[0]))
        time.sleep(.3)
    s.probe.text = text


def visible(s, pattern, tag, timeout=90):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        frame = s.frame(tag)
        text = fltk.tesseract_ocr(frame, s.output, tag)
        if re.search(pattern, re.sub(r'\s+', '', text), re.I): return text
        if s.process.poll() is not None: raise RuntimeError('QEMU exited')
        time.sleep(.5)
    raise AssertionError(f'Visible state missing: {pattern}')


def operation(s, mode, offset, timeout=120):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        suffix = s.serial.read_text(errors='replace')[offset:]
        match = re.search(rf'RELIEFAPK_OPERATION mode={mode} exit=(\d+)', suffix)
        if match:
            assert match[1] == '0', f'CA operation failed: {match[0]}'
            return
        if s.process.poll() is not None: raise RuntimeError('QEMU exited')
        time.sleep(.2)
    raise AssertionError(f'CA operation mode {mode} did not finish')


def exercise(s, imap_port):
    desktop_settled(s)
    command(s, 'mailcatest', '[reliefmail.elf] FLTK mail window ready')
    log = s.serial.read_text(errors='replace')
    s.pid = int(re.findall(r'MAIL_CA_START pid=(\d+)', log)[-1])
    s.geometry = fltk.wait_client_geometry(s.serial, s.process, s.pid, 0)
    visible(s, r'统一收件箱.*0.*封', 'mail-profile-ready')
    s.screenshot('reliefmail-main')
    s.click(210, 17)
    s.click(440, 64)
    # A mapped window/title can precede the first drawn modal. Wait for its
    # actual contents before deriving coordinates from the active title.
    visible(s, r'服务商.*显示名称.*邮箱地址', 'account-ready')
    return exercise_account(s, imap_port)


def exercise_account(s, imap_port):
    x, y = dialog_origin(s.frame('account-dialog'), 800, 548)
    def fill(px, py, text):
        s.probe.click(x + px, y + py)
        s.fill(text)
    fill(270, 146, 'CA regression')
    fill(620, 146, 'alice@reliefmail.test')
    s.probe.click(x + 166, y + 28)
    fill(370, 100, 'mail.reliefmail.test')
    fill(180, 140, '465')
    s.probe.click(x + 103, y + 28)
    fill(370, 100, 'mail.reliefmail.test')
    fill(180, 140, str(imap_port))
    fill(370, 220, 'alice@reliefmail.test')
    fill(370, 256, 'phase-a-fixture-password')
    s.probe.click(x + 75, y + 512)
    visible(s, r'无法加载系统CA', 'ca-failure-visible')
    s.screenshot('reliefmail-ca-error')
    # The failure dialog offers repair without changing account CA settings.
    error_text = re.sub(r'\s+', '', fltk.tesseract_ocr(s.frame('ca-failure-visible'), s.output, 'ca-failure'))
    assert 'CA' in error_text and ('修复' in error_text or '证书' in error_text), error_text
    return exercise_repair(s)


def exercise_repair(s):
    offset = len(s.serial.read_text(errors='replace'))
    s.probe.key('ret')
    operation(s, 1, offset)
    visible(s, r'证书恢复检查完成', 'recovery-plan-painted')
    s.screenshot('reliefmail-ca-recovery-plan')
    offset = len(s.serial.read_text(errors='replace'))
    s.probe.key('ret')
    xorg.wait_serial_after(s.serial, 'RELIEFAPK_AUTH requested', s.process, offset, timeout=120)
    s.screenshot('reliefmail-ca-authentication')
    s.probe.text('test')
    s.probe.key('ret')
    operation(s, 4, offset, timeout=480)
    visible(s, r'已[成成功加]+完成', 'recovery-completion-painted')
    s.screenshot('reliefmail-ca-repaired')
    # Use visible window controls; Mail's modal account dialog can reclaim
    # keyboard focus when sudo exits. Alt+F4 could otherwise hit the wrong app.
    tool_pid = re.findall(r'exec pid=(\d+) path=/usr/lib/reliefos/apps/reliefapk/reliefapk.elf',
                          s.serial.read_text(errors='replace'))[0]
    s.probe.click(783, 731)  # Completed operation's Close, on the fixed 1280x800 guest.
    time.sleep(1)
    offset = len(s.serial.read_text(errors='replace'))
    s.probe.click(1264, 67)  # ReliefAPK title-bar Close.
    xorg.wait_serial_after(s.serial, f'pid={tool_pid} name=reliefapk.elf code=0', s.process, offset, timeout=60)
    return exercise_retry(s)


def exercise_retry(s):
    visible(s, r'IMAP.*密码.*授权码', 'account-returned')
    x, y = dialog_origin(s.frame('account-after-repair'), 800, 548)
    s.probe.click(x + 370, y + 256)
    s.fill('phase-a-fixture-password')
    s.probe.click(x + 75, y + 512)
    visible(s, r'收件认证成功', 'mail-retry-success', timeout=120)
    s.screenshot('reliefmail-ca-retry-success')
    s.probe.click(x + 221, y + 28)
    visible(s, r'修复或更新系统证书', 'account-certificates-painted')
    return exercise_finish(s)


def exercise_finish(s):
    x, y = dialog_origin(s.frame('account-certificates-final'), 800, 548)
    s.screenshot('reliefmail-certificates')
    s.probe.click(x + 600, y + 512)  # Discard fixture account, not the system CA update.
    time.sleep(.5)
    offset = len(s.serial.read_text(errors='replace'))
    mx, my, mw, _mh = s.geometry
    s.probe.click(mx + mw - 10, my - 10)
    xorg.wait_serial_after(s.serial, 'MAIL_CA_EXIT status=0', s.process, offset, timeout=90)
    return exercise_public_tls(s, (x, y))


def exercise_public_tls(s, origin):
    s.probe.click(25, 310)
    time.sleep(1.5)  # Let IceWM deliver terminal focus before the first slash.
    s.probe.key('ctrl-u')
    public = {}
    for host, port in [('imap.163.com', 993), ('smtp.163.com', 465)]:
        offset = len(s.serial.read_text(errors='replace'))
        command(s, f'/usr/lib/reliefos/tests/mail-tls-probe {host} {port} >/dev/ttyS0 2>&1',
                'MAIL_TLS_PROBE_', timeout=90)
        suffix = s.serial.read_text(errors='replace')[offset:]
        public[host] = {'verified': f'MAIL_TLS_PROBE_OK host={host}' in suffix,
                        'log': suffix[suffix.find('MAIL_TLS_PROBE_'):].strip()[:1024]}
    return {'passed': True, 'platform': 'QEMU KVM / ReliefOS Xorg / IceWM',
            'account_dialog_origin': list(origin), 'failure_visible': True,
            'signed_ca_repair': True, 'imap_retry_verified': True,
            'public_tls_probes': public, 'vmware_verified': False,
            'default_vmdk_boot_verified': False}


def verify_disk(output, metadata):
    post = output / 'post-run'
    post.mkdir()
    root = next(part for part in metadata['gpt_partitions'] if part['name'] == 'RELIEFOS_ROOT')
    layout = {'root_first': int(root['first']), 'root_last': int(root['last'])}
    image = fltk.vdso.extract_root(Path(metadata['private_image']), layout, post)
    def read(path):
        return subprocess.check_output(['debugfs', '-R', 'cat ' + path, str(image)], stderr=subprocess.DEVNULL)
    final_bundle = read('/etc/ssl/certs/ca-certificates.crt')
    def fingerprints(pem):
        return {hashlib.sha256(ssl.PEM_cert_to_DER_cert(part.decode())).hexdigest()
                for part in re.findall(br'-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----', pem, re.S)}
    actual_roots = fingerprints(final_bundle)
    for name in ['ca.crt', 'untrusted-ca.crt']:
        assert fingerprints((output / 'certificates' / name).read_bytes()) <= actual_roots
    for name, source in [('reliefmail-https-test.crt', 'ca.crt'),
                         ('reliefmail-imap-test.crt', 'untrusted-ca.crt')]:
        assert read('/usr/local/share/ca-certificates/' + name) == (output / 'certificates' / source).read_bytes()
    installed = read('/lib/apk/db/installed').decode()
    packages = [block for block in installed.split('\n\n') if '\nP:ca-certificates-bundle\n' in '\n' + block + '\n']
    assert len(packages) == 1 and f'\nV:{CA_VERSION}\n' in '\n' + packages[0] + '\n'
    source_packages = [block for block in installed.split('\n\n') if '\nP:ca-certificates\n' in '\n' + block + '\n']
    assert len(source_packages) == 1 and f'\nV:{CA_VERSION}\n' in '\n' + source_packages[0] + '\n'
    listing = subprocess.check_output(['debugfs', '-R', 'ls -p /etc/ssl/certs', str(image)],
                                     stderr=subprocess.DEVNULL).decode()
    backups = re.findall(r'/([^/\n]*\.reliefapk-ca-backup[^/\n]*)/', listing)
    assert backups, listing
    assert any(read('/etc/ssl/certs/' + name) == (output / 'broken-ca').read_bytes() for name in backups)
    assert fltk.sha256(Path(metadata['source_image'])) == metadata['source_image_sha256_before']
    guard = output / 'final-guard-artifacts.json'
    final_program = {}
    if guard.exists():
        expected = json.loads(guard.read_text())['binary_sha256']
        actual = hashlib.sha256(read('/usr/lib/reliefos/apps/reliefapk/reliefapk.elf')).hexdigest()
        assert actual == expected, (actual, expected)
        final_program['final_apk_sha256'] = actual
    return {**final_program, 'ca_package_version': CA_VERSION, 'ca_sources_version': CA_VERSION,
            'generated_root_count': len(actual_roots), 'source_image_unchanged': True,
            'ca_sha256': hashlib.sha256(final_bundle).hexdigest(),
            'old_ca_backups': backups}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--interactive', action='store_true')
    args = parser.parse_args()
    output = args.output.resolve()
    if not output.is_relative_to(ROOT / 'out') or output.exists():
        parser.error('output must be a fresh repository out subdirectory')
    output.mkdir(parents=True)
    listener, record, server, bundle, public = fixtures(output)
    s = None
    result = None
    try:
        metadata = prepare(args.image.resolve(), output, bundle, public, server.server_port)
        s = Session(output)
        keyboard(s)
        s.login()
        threading.Thread(target=protocol._serve_one,
            args=('imap-login', listener, record, 900.0), daemon=True).start()
        print('Mail CA guest desktop ready', flush=True)
        result = exercise(s, listener.getsockname()[1])
        if args.interactive:
            console = {**globals(), **locals()}
            code.interact(banner='Mail CA acceptance: continue verification in this owned VM.', local=console)
            result = console.get('result')
    except Exception:
        traceback.print_exc()
        if s: s.frame('verification-failure')
        if args.interactive:
            console = {**globals(), **locals()}
            code.interact(banner='Mail CA acceptance diagnostic session.', local=console)
            result = console.get('result')
        else: raise
    finally:
        if s: s.close()
        server.shutdown()
        server.server_close()
        listener.close()
        (output / 'imap-audit.json').write_text(json.dumps(record, indent=2) + '\n')
    if result is None: raise RuntimeError('Mail CA verification did not complete')
    result.update(verify_disk(output, metadata))
    assert record['auth'] and all(item.get('tls') for item in record['auth']), record
    assert not record['data_started'] and not record['append_count']
    assert 'phase-a-fixture-password' not in s.serial.read_text(errors='replace')
    assert any(path.endswith('.apk') for path in server.request_paths), server.request_paths
    result['https_requests'] = server.request_paths
    result['authentication_encrypted'] = True
    (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
