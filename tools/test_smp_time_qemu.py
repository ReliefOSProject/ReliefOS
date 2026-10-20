#!/usr/bin/env python3
"""Test pinned compute workers and USER_HZ on a private kernel-updated image."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

import test_vdso_qemu as disk
from run_gcc_probe_qemu import qmp_quit

ROOT = Path(__file__).resolve().parents[1]
PROBE = '/usr/lib/reliefos/tests/smp-time'
SERVICE = '/etc/init.d/reliefos-smp-time'
LINK = '/etc/runlevels/default/reliefos-smp-time'
ENV = {**os.environ, 'TMPDIR': str(ROOT / 'out/tmp')}


def run(command, **kwargs):
    return subprocess.run([str(x) for x in command], cwd=ROOT, env=ENV,
                          check=True, **kwargs)


def compile_probe(out: Path, sysroot: Path) -> Path:
    resource = Path(subprocess.check_output(['clang', '-print-resource-dir'], text=True).strip())
    obj, binary = out / 'probe.o', out / 'smp-time'
    run(['clang', '--target=x86_64-linux-musl', '-nostdinc',
         '-isystem', sysroot / 'include', '-isystem', resource / 'include',
         '-O2', '-Wall', '-Wextra', '-Werror', '-fno-stack-protector',
         '-mno-avx', '-mno-avx2', '-c', ROOT / 'tools/tests/smp_time_guest_test.c', '-o', obj])
    run(['ld.lld', '-static', '--build-id=none', '-z', 'max-page-size=0x1000',
         '--image-base=0x4000000', '-o', binary, sysroot / 'lib/crt1.o',
         sysroot / 'lib/crti.o', obj, sysroot / 'lib/mimalloc.o', '--start-group',
         sysroot / 'lib/libc.a', '--end-group', sysroot / 'lib/crtn.o'])
    return binary


def prepare(image: Path, kernel: Path, sysroot: Path, out: Path) -> Path:
    source_hash = disk.file_sha256(image)
    raw = out / 'guest-private.raw'
    with (out / 'stage.log').open('w') as log:
        if image.suffix == '.raw':
            run(['cp', '--reflink=auto', '--sparse=always', image, raw], stdout=log, stderr=log)
        else:
            run(['qemu-img', 'convert', '-f', 'vmdk', '-O', 'raw', image, raw], stdout=log, stderr=log)
        partitions = disk.gpt_partitions(raw)
        esp = next(p for p in partitions if p['name'] == 'RELIEFOS_ESP')
        root = next(p for p in partitions if p['name'] == 'RELIEFOS_ROOT')
        kernels = disk.install_private_kernel(raw, esp['first'], kernel, out)
        layout = {'root_first': root['first'], 'root_last': root['last']}
        root_image = disk.extract_root(raw, layout, out)
        binary = compile_probe(out, sysroot)
        service_file = out / 'reliefos-smp-time'
        service_file.write_text('''#!/sbin/openrc-run
description="ReliefOS SMP CPU-time validation"
depend() { after localmount; before reliefos-session; }
start() {
    /usr/lib/reliefos/tests/smp-time > /dev/ttyS0 2>&1
    status=$?
    echo "[smp-time-runner] DONE status=$status" > /dev/ttyS0
    return 0
}
''', encoding='ascii')
        commands = []
        disk.add_missing_root_directory(root_image, str(Path(PROBE).parent), commands)
        for destination in (PROBE, SERVICE, LINK):
            if disk.debugfs_exists(root_image, destination): commands.append(f'rm {destination}')
        for source, destination in ((binary, PROBE), (service_file, SERVICE)):
            commands += [f'write "{source}" {destination}', f'set_inode_field {destination} mode 0100755']
        commands.append(f'symlink {LINK} {SERVICE}')
        batch = out / 'debugfs.commands'
        batch.write_text('\n'.join(commands) + '\n')
        run(['debugfs', '-w', '-f', batch, root_image], stdout=log, stderr=log)
        payload = []
        for i, (source, destination) in enumerate(((binary, PROBE), (service_file, SERVICE))):
            readback = out / f'payload-readback-{i}'
            run(['debugfs', '-R', f'dump {destination} {readback}', root_image], stdout=log, stderr=log)
            expected = disk.file_sha256(source)
            if disk.file_sha256(readback) != expected: raise RuntimeError('payload readback mismatch')
            payload.append({'guest_path': destination, 'sha256': expected})
        run(['e2fsck', '-fn', root_image], stdout=log, stderr=log)
        disk.put_root_back(raw, root_image, layout)
    if disk.file_sha256(image) != source_hash: raise RuntimeError('input image was modified')
    manifest = {'source': str(image), 'source_sha256_before_after': source_hash,
        'source_unchanged': True, 'kernel': str(kernel), 'kernel_readbacks': kernels,
        'partitions': partitions, 'payload': payload,
        'raw_sha256': disk.file_sha256(raw), 'probe_source_sha256': disk.file_sha256(ROOT / 'tools/tests/smp_time_guest_test.c'),
        'controller_sha256': disk.file_sha256(Path(__file__))}
    (out / 'image-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    raw.chmod(0o400)
    return raw


def boot(raw: Path, out: Path, cpus: int, accelerator: str, timeout: float) -> dict:
    case = out / f'smp{cpus}'
    case.mkdir()
    serial = case / 'serial.log'
    record = {'cpus': cpus, 'accelerator': accelerator, 'completed': False,
              'raw_sha256': disk.file_sha256(raw)}
    started = time.monotonic()
    # AF_UNIX paths have a small fixed limit; long artifact names must not
    # prevent QEMU startup. Keep the owned socket in a short private directory.
    with tempfile.TemporaryDirectory(prefix='smp-qmp-', dir=ROOT / 'out/tmp') as temporary, \
            (case / 'qemu.log').open('w') as log:
        qmp = Path(temporary) / 'qmp'
        command = disk.qemu_command(raw, serial, qmp, cpus, accelerator, None, False)
        command += ['-snapshot']
        process = subprocess.Popen(command, cwd=ROOT, env=ENV, stdout=log, stderr=log)
        try:
            deadline = started + timeout
            while time.monotonic() < deadline:
                text = serial.read_text(errors='replace') if serial.exists() else ''
                if 'BUGCHECK' in text or '[PANIC]' in text: raise RuntimeError('guest fatal marker')
                if '[smp-time-runner] DONE' in text: break
                if process.poll() is not None: raise RuntimeError('QEMU exited before probe completion')
                time.sleep(0.2)
            else: raise TimeoutError('guest probe did not finish')
            record['probe_done'] = re.findall(r'\[smp-time\] DONE failures=(\d+) raw_irq_probe=(\d+)', text)
            record['runner_status'] = re.findall(r'\[smp-time-runner\] DONE status=(\d+)', text)
            record['calibrations'] = re.findall(r'LAPIC timer APIC=(\d+) target_hz=(\d+) count=(\d+) (\S+) clock=acpi-pm window_us=(\d+) start=(\d+) end=(\d+) polls=(\d+)', text)
            record['calibration_window_unit'] = 'microseconds from ACPI PM timer'
            record['cpu_samples'] = re.findall(r'\[smp-time\] CPU cpu=(\d+) total=(\d+) busy=(\d+) wall_us=(\d+)(?: raw_timer_events=(\d+))?', text)
            if record['probe_done'] not in [[('0', '0')], [('0', '1')]] or record['runner_status'] != ['0']:
                raise RuntimeError('guest probe failed')
            if len(record['cpu_samples']) != cpus: raise RuntimeError('not all CPUs sampled')
            if len(record['calibrations']) != cpus - 1 or any(v[3] != 'calibrated' for v in record['calibrations']):
                raise RuntimeError('AP timer calibration missing or fell back')
            record['completed'] = True
        except BaseException as error:
            record['error'] = repr(error)
            raise
        finally:
            if process.poll() is None:
                try: qmp_quit(qmp, process)
                except Exception:
                    process.terminate()
                    try: process.wait(timeout=5)
                    except subprocess.TimeoutExpired: process.kill(); process.wait(timeout=5)
            record['qemu_exit'] = process.poll()
            record['elapsed_seconds'] = time.monotonic() - started
            (case / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--sysroot', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--prepare-only', action='store_true')
    parser.add_argument('--cpus', type=int, nargs='+', choices=(1, 2, 4), default=[1, 2, 4])
    parser.add_argument('--accelerator', choices=('auto', 'kvm', 'tcg'), default='auto')
    parser.add_argument('--timeout', type=float, default=180)
    args = parser.parse_args()
    out = args.out.resolve(); out.mkdir(parents=True, exist_ok=False)
    raw = prepare(args.image.resolve(), args.kernel.resolve(), args.sysroot.resolve(), out)
    print('PREPARED', raw, flush=True)
    if not args.prepare_only:
        for cpus in args.cpus:
            result = boot(raw, out, cpus, disk.choose_accelerator(args.accelerator), args.timeout)
            print(f'PASS QEMU SMP={cpus} elapsed={result["elapsed_seconds"]:.2f}s', flush=True)


if __name__ == '__main__':
    main()
