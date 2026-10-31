#!/usr/bin/env python3
"""Operate the real ReliefDiagram ELF and independently read back saved documents."""
from pathlib import Path
import argparse
import code
import json
import os
import re
import subprocess
import time
import traceback
import xml.etree.ElementTree as ET
from PIL import Image
import test_fltk_apps_qemu as fltk
import test_vdso_qemu as vdso
import test_xorg_vm_qemu as xorg
from test_reliefmath_qemu import Session, filename
from qmp_terminal_smoke import text_keys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'out/xorg-vmware-debug'


def prepare(image, kernel, output):
    output.mkdir(parents=True, exist_ok=False)
    fixtures = output / 'fixtures'
    fixtures.mkdir()
    core = ROOT / 'userland/apps/reliefdiagram'
    command = ['c++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
               '-nostdinc++', '-fno-exceptions', '-fno-rtti',
               str(ROOT / 'tools/tests/reliefdiagram_guest_verify.cpp'),
               *(str(core / name) for name in ('model.cpp', 'document.cpp', 'scene.cpp')),
               '-lm', '-o', str(output / 'guest-verify')]
    subprocess.run(command, check=True)
    subprocess.run([str(output / 'guest-verify'), 'prepare', str(fixtures)], check=True)
    probe = fltk.target_compile_probe(output, OUT / 'sysroot/musl', 'clang++', 'ld.lld')
    runner = output / 'rd-runner'
    runner.write_text('''#!/bin/sh
export LC_ALL=zh_CN.UTF-8 LANG=zh_CN.UTF-8
/usr/lib/reliefos/apps/reliefdiagram/reliefdiagram.elf "$@" >/dev/ttyS0 2>&1 &
pid=$!
echo "RD_START pid=$pid" >/dev/ttyS0
/usr/lib/reliefos/tests/fltk-ui-probe --watch-window-focus "$pid" >/dev/ttyS0 2>&1 &
watcher=$!
wait "$pid"
status=$?
kill "$watcher" 2>/dev/null
echo "RD_EXIT status=$status" >/dev/ttyS0
''')
    entry = output / 'entry'
    entry.write_text('#!/bin/sh\nexec /usr/lib/reliefos/apps/reliefdiagram/reliefdiagram.elf "$@"\n')
    elf = OUT / 'userland/reliefdiagram.elf'
    fltk.validate_target_elf(elf, sysroot=OUT / 'sysroot/musl', required_needed=('libfltk.so.1.3',))
    payload = [(elf, '/usr/lib/reliefos/apps/reliefdiagram/reliefdiagram.elf', 0o100755),
               (ROOT / 'resources/build-art/app-icons/reliefdiagram.bmp', '/usr/lib/reliefos/apps/reliefdiagram/reliefdiagram.bmp', 0o100644),
               (OUT / 'generated/nls/zh_CN/LC_MESSAGES/leonos.mo', fltk.LOCALE_GUEST, 0o100644),
               (ROOT / 'userland/apps/reliefdiagram/reliefdiagram.desktop', '/usr/share/applications/reliefos-reliefdiagram.desktop', 0o100644),
               (ROOT / 'system/rootfs/usr/share/mime/packages/reliefdiagram.xml', '/usr/share/mime/packages/reliefdiagram.xml', 0o100644),
               (probe, fltk.PROBE_GUEST, 0o100755), (runner, '/usr/bin/rdtest', 0o100755),
               (entry, '/usr/bin/reliefdiagram', 0o100755),
               (fixtures / 'invalid.rdiagram', '/home/test/invalid.rdiagram', 0o100644)]
    for size in (16, 32, 48, 64, 128, 256):
        icon = f'hicolor/{size}x{size}/apps/reliefos-reliefdiagram.png'
        payload.append((ROOT / 'resources/build-art/app-icons' / icon, '/usr/share/icons/' + icon, 0o100644))
    metadata = fltk.prepare_disk(image, kernel, output, payload)
    (output / 'prepare.json').write_text(json.dumps(metadata, indent=2) + '\n')
    return metadata


def explicit_keys(s):
    def key(value):
        codes = value.split('-')
        events = [{'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': item}}}
                  for down, seq in ((True, codes), (False, reversed(codes))) for item in seq]
        s.probe.command('input-send-event', {'events': events})
        time.sleep(.16)
    def text(value):
        for ch in value:
            key(text_keys(ch)[0])
        time.sleep(.4)
    s.probe.key, s.probe.text = key, text


def wait(s, marker, offset=0, timeout=90):
    xorg.wait_serial_after(s.serial, marker, s.process, offset, timeout=timeout)


def launch(s, path=''):
    s.launch_offset = len(s.serial.read_text(errors='replace'))
    s.probe.click(300, 200)
    time.sleep(1.5)
    s.probe.key('ctrl-u')
    s.probe.text('rdtest' + (' ' + path if path else ''))
    s.probe.key('ret')
    wait(s, '[reliefdiagram.elf] FLTK diagram workspace ready', s.launch_offset)
    log = s.serial.read_text(errors='replace')[s.launch_offset:]
    s.pid = int(re.search(r'RD_START pid=(\d+)', log)[1])
    s.geometry = fltk.wait_client_geometry(s.serial, s.process, s.pid, s.launch_offset)
    if path:
        wait(s, '[reliefdiagram.elf] opened', s.launch_offset)
    time.sleep(1)


def edit_text(s, value):
    s.click(s.geometry[2] - 150, 261)
    s.fill(value)
    s.probe.key('ctrl-ret')
    time.sleep(.4)
    s.wait_text('属性已应用','properties-applied',timeout=20)


def save(s, name):
    offset = len(s.serial.read_text(errors='replace'))
    s.probe.key('ctrl-s')
    filename(s, '/home/test/' + name, 'save-chooser')
    wait(s, '[reliefdiagram.elf] saved', offset)


def export(s, name, x, y):
    offset = len(s.serial.read_text(errors='replace'))
    s.click(x, y)
    filename(s, '/home/test/' + name, 'export-chooser')
    wait(s, '[reliefdiagram.elf] export complete', offset)


def flow_point(s, index, corner=False):
    data = json.loads((s.output / 'fixtures/template-1.json').read_text())
    x, y, w, h = data['nodes'][index]
    bx, by, bw, bh = data['bounds']
    cw, ch = s.geometry[2] - 430, s.geometry[3] - 256
    zoom = max(.15, min(1.5, (cw - 64) / bw, (ch - 64) / bh))
    return (round(172 + cw / 2 + (x + w * (1 if corner else .5) - bx - bw / 2) * zoom),
            round(220 + ch / 2 + (y + h * (1 if corner else .5) - by - bh / 2) * zoom))


def drag(s, point, dx, dy, kind='move'):
    offset = len(s.serial.read_text(errors='replace'))
    sx, sy = point
    gx, gy, _, _ = s.geometry
    s.probe.move(gx + sx, gy + sy)
    time.sleep(.4)
    s.probe.command('input-send-event', {'events': [{'type': 'btn', 'data': {'down': True, 'button': 'left'}}]})
    time.sleep(.2)
    for n in range(1, 6):
        s.probe.move(gx + sx + round(dx * n / 5), gy + sy + round(dy * n / 5))
        time.sleep(.15)
    time.sleep(.4)
    s.probe.command('input-send-event', {'events': [{'type': 'btn', 'data': {'down': False, 'button': 'left'}}]})
    time.sleep(.6)
    wait(s, '[reliefdiagram.elf] gesture ' + kind, offset, timeout=15)


def close_app(s):
    offset = len(s.serial.read_text(errors='replace'))
    x, y, w, h = s.geometry
    for _ in range(2):
        s.probe.click(x + w - 10, y - 11)
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            if 'RD_EXIT status=0' in s.serial.read_text(errors='replace')[offset:]:
                return
            time.sleep(.2)
    raise AssertionError('ReliefDiagram did not exit cleanly')


def exercise(s):
    launch(s)
    s.click(65, 477)
    time.sleep(1.5)
    s.frame('reliefdiagram-mind-map')
    edit_text(s, 'Diagram review')
    s.click(60, 118)
    edit_text(s, 'Timeline')
    s.click(65, 477)
    s.click(248, 118)
    time.sleep(1)
    s.frame('reliefdiagram-folded')
    s.click(248, 118)
    s.probe.key('ctrl-z')
    s.probe.key('ctrl-y')
    save(s, 'acceptance.rdiagram')
    s.frame('reliefdiagram-edited')
    export(s, 'diagram.svg', 544, 50)
    export(s, 'diagram.png', 642, 50)
    s.click(235, 86)
    export(s, 'diagram.md', 766, 120)
    s.click(275, 120)
    s.click(60, 86)
    time.sleep(1.5)
    s.frame('reliefdiagram-flowchart')
    s.click(875, 118)
    s.click(*flow_point(s, 0))
    s.click(*flow_point(s, 6))
    edit_text(s, 'summary')
    s.probe.key('esc')
    s.click(978, 149)
    drag(s, flow_point(s, 3), 40, 20)
    s.probe.key('ctrl-z')
    s.probe.key('ctrl-y')
    s.click(*flow_point(s,2))
    drag(s,flow_point(s,2,corner=True),24,24,kind='resize')
    s.probe.key('ctrl-a')
    s.probe.key('ctrl-c')
    s.probe.key('ctrl-v')
    time.sleep(1)
    s.frame('reliefdiagram-copied')
    s.probe.key('ctrl-z')
    s.probe.key('ctrl-y')
    s.probe.key('ctrl-z')
    save(s, 'flow.rdiagram')
    s.frame('reliefdiagram-flow-edited')
    s.click(235, 86)
    s.click(450, 120)
    save(s, 'logic.rdiagram')
    s.click(60, 86)
    s.frame('reliefdiagram-logic')
    close_app(s)
    launch(s)
    s.click(65,477)
    time.sleep(2)
    s.frame('reliefdiagram-mind-map-final')
    close_app(s)
    launch(s, '/home/test/acceptance.rdiagram')
    s.frame('reliefdiagram-reopened')
    s.probe.key('ctrl-o')
    filename(s, '/home/test/invalid.rdiagram', 'invalid-chooser')
    time.sleep(1)
    s.frame('reliefdiagram-invalid-rejected')
    s.probe.key('ret')
    time.sleep(.5)
    close_app(s)


def verify(output, metadata):
    folder = output / 'readback'
    folder.mkdir(exist_ok=True)
    part = next(p for p in metadata['gpt_partitions'] if p['name'] == 'RELIEFOS_ROOT')
    image = folder / 'root.ext4'
    subprocess.run(['dd', 'if=' + str(output / 'guest-private.raw'), 'of=' + str(image),
                    'bs=512', 'skip=' + str(part['first']),
                    'count=' + str(int(part['last']) - int(part['first']) + 1),
                    'conv=sparse', 'status=none'], check=True)
    names = ('acceptance.rdiagram', 'flow.rdiagram', 'logic.rdiagram', 'diagram.svg', 'diagram.png', 'diagram.md')
    for name in names:
        subprocess.run(['debugfs', '-R', f'dump /home/test/{name} {fltk.debugfs_quote(folder / name)}', str(image)], check=True)
        assert (folder / name).is_file(), name
    subprocess.run([str(output / 'guest-verify'), 'verify', str(folder)], check=True)
    ET.parse(folder / 'diagram.svg')
    assert 'Timeline' in (folder / 'diagram.svg').read_text()
    assert 'Timeline' in (folder / 'diagram.md').read_text()
    with Image.open(folder / 'diagram.png') as picture:
        picture.load()
        assert picture.width > 200 and picture.height > 200
    assert fltk.sha256(Path(metadata['source_image'])) == metadata['source_image_sha256_before']
    result = {'passed': True, 'platform': 'QEMU KVM / ReliefOS Xorg', 'vmware_verified': False,
              'source_image_unchanged': True, 'readbacks': {name: fltk.sha256(folder / name) for name in names}}
    (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--kernel', type=Path, default=fltk.DEFAULT_KERNEL)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--interactive', action='store_true', help='keep the owned VM available after a failed assertion')
    args = parser.parse_args()
    output = args.output.resolve()
    metadata = prepare(args.image.resolve(), args.kernel.resolve(), output)
    s = None
    try:
        s = Session(output)
        explicit_keys(s)
        s.login()
        print('QEMU desktop login complete', flush=True)
        exercise(s)
    except Exception:
        traceback.print_exc()
        if s:
            s.frame('verification-failure')
        if args.interactive:
            code.interact(local=locals())
        else:
            raise
    finally:
        if s:
            s.close()
    verify(output, metadata)


if __name__ == '__main__':
    main()
