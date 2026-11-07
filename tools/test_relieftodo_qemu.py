#!/usr/bin/env python3
"""Real ReliefTodo GUI acceptance in a hash-checked, private QEMU image."""
from pathlib import Path
import argparse
import code
import csv
import json
import os
import re
import subprocess
import time

import test_fltk_apps_qemu as fltk
import test_vdso_qemu as vdso
import test_xorg_vm_qemu as xorg
from test_reliefmath_qemu import Session as MathSession, filename, resize, wait_focus

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'out/xorg-vmware-debug'


def prepare(image, kernel, output):
    output.mkdir(parents=True, exist_ok=False)
    env = {**os.environ, 'TMPDIR': str(ROOT / 'out/tmp')}
    core = ROOT / 'userland/apps/relieftodo'
    with (output / 'verifier-build.log').open('w') as log:
        subprocess.run(['c++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-nostdinc++', '-fno-exceptions', '-fno-rtti',
                        str(ROOT / 'tools/tests/relieftodo_host_verify.cpp'),
                        *[str(core / name) for name in ('model.cpp', 'document.cpp', 'file_io.cpp')],
                        '-o', str(output / 'guest-verify')], cwd=ROOT, env=env,
                       stdout=log, stderr=log, check=True)
    probe = fltk.target_compile_probe(output, OUT / 'sysroot/musl', 'clang++', 'ld.lld')
    template = json.loads((output / 'probe-build.command.json').read_text())
    objects = []
    with (output / 'core-probe-build.log').open('w') as log:
        for source in (ROOT / 'tools/tests/relieftodo_host_verify.cpp',
                       *[core / name for name in ('model.cpp', 'document.cpp', 'file_io.cpp')]):
            obj = output / ('core-' + source.stem + '.o')
            command = list(template['compile'])
            command[-3], command[-1] = str(source), str(obj)
            subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=log, check=True)
            objects.append(str(obj))
        command = list(template['link'])
        index = command.index(str(output / 'fltk_ui_probe.o'))
        command[index:index + 1] = objects
        command[command.index(str(probe))] = str(output / 'core-probe.elf')
        subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=log, check=True)
    core_runner = output / 'rt-core-runner'
    core_runner.write_text('''#!/bin/sh
if TMPDIR=/home/test /usr/lib/reliefos/tests/relieftodo-core-probe >/dev/ttyS0 2>&1; then
 echo RT_CORE_PASS >/dev/ttyS0
else
 echo RT_CORE_FAIL >/dev/ttyS0
 exit 1
fi
''')
    runner = output / 'rt-runner'
    runner.write_text('''#!/bin/sh
export LC_ALL=zh_CN.UTF-8 LANG=zh_CN.UTF-8
/usr/lib/reliefos/apps/relieftodo/relieftodo.elf "$@" >/dev/ttyS0 2>&1 &
pid=$!
echo "RT_START pid=$pid" >/dev/ttyS0
/usr/lib/reliefos/tests/fltk-ui-probe --watch-window-focus "$pid" >/dev/ttyS0 2>&1 &
probe_pid=$!
wait "$pid"
status=$?
kill "$probe_pid" 2>/dev/null
echo "RT_EXIT status=$status" >/dev/ttyS0
''')
    entry = output / 'relieftodo-entry'
    entry.write_text('#!/bin/sh\nexec /usr/lib/reliefos/apps/relieftodo/relieftodo.elf "$@"\n')
    fileman_runner = output / 'rt-fileman-runner'
    fileman_runner.write_text('''#!/bin/sh
export LC_ALL=zh_CN.UTF-8 LANG=zh_CN.UTF-8
mkdir -p /home/test/todo-association || exit 1
cp /home/test/saved.rtodo /home/test/todo-association/saved.rtodo || exit 1
/usr/lib/reliefos/apps/fileman/fileman.elf /home/test/todo-association >/dev/ttyS0 2>&1 &
pid=$!
echo "RTFM_START pid=$pid" >/dev/ttyS0
/usr/lib/reliefos/tests/fltk-ui-probe --watch-window-focus "$pid" >/dev/ttyS0 2>&1 &
probe_pid=$!
(
 while kill -0 "$pid" 2>/dev/null; do
  child=$(pgrep -P "$pid")
  if [ -n "$child" ]; then
   echo "RTFM_CHILD pid=$child" >/dev/ttyS0
   /usr/lib/reliefos/tests/fltk-ui-probe --watch-window-focus "$child" >/dev/ttyS0 2>&1
   echo "RTFM_CHILD_WINDOW_CLOSED" >/dev/ttyS0
   exit
  fi
  sleep 1
 done
) &
child_probe=$!
wait "$pid"
status=$?
kill "$probe_pid" "$child_probe" 2>/dev/null
echo "RTFM_EXIT status=$status" >/dev/ttyS0
''')
    payload = [
        (OUT / 'userland/relieftodo.elf', '/usr/lib/reliefos/apps/relieftodo/relieftodo.elf', 0o100755),
        (OUT / 'userland/fileman.elf', '/usr/lib/reliefos/apps/fileman/fileman.elf', 0o100755),
        (OUT / 'generated/nls/zh_CN/LC_MESSAGES/leonos.mo', fltk.LOCALE_GUEST, 0o100644),
        (core / 'relieftodo.desktop', '/usr/share/applications/reliefos-relieftodo.desktop', 0o100644),
        (ROOT / 'system/rootfs/usr/share/mime/packages/relieftodo.xml', '/usr/share/mime/packages/relieftodo.xml', 0o100644),
        (ROOT / 'resources/build-art/app-icons/relieftodo.bmp', '/usr/lib/reliefos/apps/relieftodo/relieftodo.bmp', 0o100644),
        (entry, '/usr/bin/relieftodo', 0o100755), (runner, '/usr/bin/rttest', 0o100755),
        (fileman_runner, '/usr/bin/rtfileman', 0o100755),
        (core_runner, '/usr/bin/rtcoretest', 0o100755),
        (output / 'core-probe.elf', '/usr/lib/reliefos/tests/relieftodo-core-probe', 0o100755),
        (probe, fltk.PROBE_GUEST, 0o100755),
    ]
    for size in (16, 32, 48, 64, 128, 256):
        path = ROOT / f'resources/build-art/app-icons/hicolor/{size}x{size}/apps/reliefos-relieftodo.png'
        payload.append((path, f'/usr/share/icons/hicolor/{size}x{size}/apps/{path.name}', 0o100644))
    fltk.validate_target_elf(OUT / 'userland/relieftodo.elf', sysroot=OUT / 'sysroot/musl',
                             required_needed=('libfltk.so.1.3',))
    original = vdso.extract_root

    def sparse(raw, layout, folder):
        root = folder / 'guest-root.ext4'
        subprocess.run(['dd', 'if=' + str(raw), 'of=' + str(root), 'bs=512',
                        'skip=' + str(layout['root_first']),
                        'count=' + str(int(layout['root_last']) - int(layout['root_first']) + 1),
                        'conv=sparse', 'status=none'], check=True)
        return root

    vdso.extract_root = sparse
    try:
        metadata = fltk.prepare_disk(image, kernel, output, payload)
    finally:
        vdso.extract_root = original
    Path(metadata['root_image_snapshot']).unlink()
    metadata['root_image_snapshot_removed'] = True
    (output / 'prepare.json').write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + '\n')
    return metadata


class Session(MathSession):
    def __init__(self, output):
        super().__init__(output)
        from qmp_terminal_smoke import text_keys

        def key(value):
            # HMP sendkey schedules release on a virtual-clock timer. Under
            # this debug guest the timer can overlap the next identical key,
            # producing an extra character. Send each complete transition
            # sequence in order, without a delayed release timer.
            codes = value.split('-')
            events = [
                {'type': 'key', 'data': {'down': down,
                 'key': {'type': 'qcode', 'data': code}}}
                for down, sequence in ((True, codes), (False, reversed(codes)))
                for code in sequence
            ]
            self.probe.command('input-send-event', {'events': events})
            with (self.output / 'keyboard.jsonl').open('a') as stream:
                stream.write(json.dumps({'time': time.monotonic(), 'events': events}) + '\n')
            time.sleep(.25)

        def text(value):
            punctuation = {',': 'comma'}
            for character in value:
                key(punctuation.get(character, text_keys(character)[0]))
            time.sleep(.5)

        self.probe.key = key
        self.probe.text = text

    def login(self):
        super().login()
        offset = len(self.serial.read_text(errors='replace'))
        self.probe.click(300, 200)
        self.probe.text('/usr/bin/rtcoretest'); self.keys('ret')
        self.wait_log('RT_CORE_PASS', offset)

    def launch(self, path=''):
        offset = len(self.serial.read_text(errors='replace'))
        self.launch_offset = offset
        self.probe.click(300, 200)
        self.probe.text('rttest' + (' ' + path if path else ''))
        self.keys('ret')
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            log = self.serial.read_text(errors='replace')[offset:]
            match = re.search(r'RT_START pid=(\d+)', log)
            if match and '[relieftodo.elf] FLTK task manager ready' in log:
                self.pid = int(match[1])
                self.geometry = fltk.wait_client_geometry(self.serial, self.process, self.pid, offset)
                self.wait_text('ReliefTodo', 'relieftodo-startup')
                return self.geometry
            if self.process.poll() is not None:
                raise RuntimeError('QEMU exited')
            time.sleep(.2)
        self.screenshot('relieftodo-launch-failed')
        raise AssertionError('ReliefTodo did not launch')

    def wait_log(self, text, offset=0, timeout=60):
        xorg.wait_serial_after(self.serial, text, self.process, offset, timeout=timeout)
        return self.serial.read_text(errors='replace')

    def close_app(self):
        offset = len(self.serial.read_text(errors='replace'))
        x, y, w, h = self.geometry
        self.probe.click(x + w - 10, y - 11)
        self.wait_log('RT_EXIT status=0', offset)


def menu(s, row):
    s.click(40, 17)
    s.click(85, 43 + 20 * row)


def check_counts(s, active, done, tag, selected=None):
    deadline = time.monotonic() + 15
    stable = None
    while time.monotonic() < deadline:
        frame = s.frame(tag)
        x, y, w, h = s.geometry
        crop = frame.crop((x + w // 2, y + h - 28, x + w - 3, y + h - 1))
        text = fltk.tesseract_ocr(crop, s.output, tag + '-counts', psm=7, scale=3)
        counts = list(map(int, re.findall(r'\d+', text)))
        if counts[:3] == [7, active, done] and (selected is None or counts[3:] == [selected]):
            if stable is None:
                stable = time.monotonic()
            if time.monotonic() - stable > .5:
                return
        else:
            stable = None
        time.sleep(.2)
    raise AssertionError(f'{tag}: unexpected summary {text!r}; expected 7/{active}/{done}/{selected}')


def check_filter(s, label, count, tag):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        frame = s.frame(tag)
        x, y, _, _ = s.geometry
        crop = frame.crop((x + 190, y + 143, x + 360, y + 180))
        readings = [
            fltk.tesseract_ocr(crop, s.output, tag + '-heading-line', psm=7, scale=3),
            fltk.tesseract_ocr(crop, s.output, tag + '-heading-sparse', psm=11, scale=3),
        ]
        normalized = ''.join(re.sub(r'\s+', '', text).replace('巳', '已') for text in readings)
        counts = [digit for text in readings for digit in re.findall(r'\d+', text)]
        if label in normalized and str(count) in counts:
            return
        time.sleep(.2)
    raise AssertionError(readings)


def fileman_association(s):
    offset = len(s.serial.read_text(errors='replace'))
    s.probe.click(300, 200)
    s.keys('ctrl-u')
    s.probe.text('/usr/bin/rtfileman')
    s.screenshot('relieftodo-file-association-command')
    s.keys('ret')
    try:
        log = s.wait_log('[fileman.elf] FLTK file manager ready', offset)
    except Exception:
        s.screenshot('relieftodo-file-association-launch-failed')
        raise
    fileman_pid = int(re.search(r'RTFM_START pid=(\d+)', log[offset:])[1])
    fileman_geometry = fltk.wait_client_geometry(s.serial, s.process, fileman_pid, offset)
    s.geometry = fileman_geometry
    # OCR often confuses the glyphs around the period in this small CJK row.
    s.wait_text(r'saved.{0,5}todo', 'relieftodo-file-association-before')
    offset = len(s.serial.read_text(errors='replace'))
    x, y, _, _ = fileman_geometry
    fltk.double_click(s.probe, x + 420, y + 149)
    log = s.wait_log('RTFM_CHILD pid=', offset)
    s.pid = int(re.search(r'RTFM_CHILD pid=(\d+)', log[offset:])[1])
    assert s.pid != fileman_pid
    s.launch_offset = offset
    s.geometry = fltk.wait_client_geometry(s.serial, s.process, s.pid, offset)
    s.wait_log('[relieftodo.elf] opened tasks=7', offset)
    s.keys('ctrl-f'); s.fill('TodoAcceptance'); time.sleep(.5)
    s.wait_text('TodoAcceptance', 'relieftodo-file-association')
    s.screenshot('relieftodo-file-association')
    x, y, w, _ = s.geometry
    s.probe.click(x + w - 10, y - 11)
    s.wait_log('RTFM_CHILD_WINDOW_CLOSED', offset)
    x, y, w, _ = fileman_geometry
    s.probe.click(x + w - 10, y - 11)
    log = s.wait_log('RTFM_EXIT status=0', offset)
    assert f'[fileman.elf] open path=/home/test/todo-association/saved.rtodo pid={s.pid}' in log[offset:]


def exercise(s):
    s.launch()
    initial_size = s.geometry[2:]
    initial = s.frame('relieftodo-overview')
    text = fltk.tesseract_ocr(initial, s.output, 'overview')
    assert fltk.cjk_count(text) > 30, 'Chinese UI was not visibly drawn'
    s.click(45, 66)  # Native ribbon: new task.
    s.fill('TodoAcceptance')
    w = s.geometry[2]
    s.click(w - 240, 274); s.fill('ReliefOS')
    s.click(w - 245, 326); s.fill('2030-01-02')
    s.click(w - 230, 416); s.fill('qemu,verified')
    s.click(w - 230, 515); s.probe.text('Persisted by real GUI')
    s.keys('ctrl-ret')
    s.click(210, 17)  # Task tools.
    s.click(55, 54)  # Urgent.
    s.click(120, 17)  # Home.
    s.screenshot('relieftodo-editing')
    s.keys('ctrl-f'); s.fill('TodoAcceptance'); time.sleep(.5)
    s.wait_text('TodoAcceptance', 'relieftodo-search')
    s.click(250, 55)  # Done.
    check_counts(s, 5, 2, 'relieftodo-completed', 1)
    s.click(250, 86)  # Restore.
    check_counts(s, 6, 1, 'relieftodo-restored', 1)
    s.click(400, 235); s.keys('ctrl-z')
    check_counts(s, 5, 2, 'relieftodo-undo', 1)
    s.keys('ctrl-y')
    check_counts(s, 6, 1, 'relieftodo-redo', 1)
    s.click(294, 17); s.click(65, 64)  # View / clear filters.
    s.click(120, 17)
    s.click(400, 235)
    # Shift range selection through the actual table widget.
    def shift(down):
        s.probe.command('input-send-event', {'events': [
            {'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': 'shift'}}}]})
    shift(True)
    try:
        s.click(400, 307)
    finally:
        shift(False)
    check_counts(s, 6, 1, 'relieftodo-multiselect', 3)
    s.click(250, 55)
    check_counts(s, 3, 4, 'relieftodo-batch-done', 3)
    s.click(250, 86)
    check_counts(s, 6, 1, 'relieftodo-batch-restored', 3)
    # Restore exactly the prior three states, keeping the original sample data.
    s.click(400, 235); s.keys('ctrl-z', 'ctrl-z')
    check_counts(s, 6, 1, 'relieftodo-batch-undone')
    s.click(70, 249); check_filter(s, '今天', 2, 'relieftodo-today')
    s.click(70, 264); check_filter(s, '即将到期', 3, 'relieftodo-upcoming')
    s.click(70, 279); check_filter(s, '已逾期', 1, 'relieftodo-overdue')
    s.click(294, 17); s.click(65, 64); s.click(120, 17)
    s.keys('ctrl-s'); filename(s, '/home/test/saved.rtodo', 'save-task-list')
    s.wait_log('[relieftodo.elf] saved tasks=7')
    for row, path in [(4, '/home/test/tasks.csv'), (5, '/home/test/tasks.md')]:
        offset = len(s.serial.read_text(errors='replace'))
        menu(s, row); filename(s, path, 'export-file')
        s.wait_log('[relieftodo.elf] export complete', offset)
    resize(s, 1000, 650)
    check_counts(s, 6, 1, 'relieftodo-minimum-window')
    resize(s, *initial_size)
    check_counts(s, 6, 1, 'relieftodo-final')
    offset = len(s.serial.read_text(errors='replace'))
    s.keys('ctrl-o'); filename(s, '/home/test/saved.rtodo', 'reopen-task-list')
    s.wait_log('[relieftodo.elf] opened tasks=7', offset)
    s.keys('ctrl-f'); s.fill('TodoAcceptance'); time.sleep(.5)
    s.screenshot('relieftodo-reopened')
    s.close_app()
    fileman_association(s)


def verify_disk(output, metadata):
    partition = next(p for p in metadata['gpt_partitions'] if p['name'] == 'RELIEFOS_ROOT')
    root = output / 'verified-root.ext4'
    subprocess.run(['dd', 'if=' + str(output / 'guest-private.raw'), 'of=' + str(root), 'bs=512',
                    'skip=' + str(partition['first']),
                    'count=' + str(partition['last'] - partition['first'] + 1),
                    'conv=sparse', 'status=none'], check=True)
    readback = output / 'readback'
    readback.mkdir(exist_ok=True)
    entries = []
    try:
        for name in ('saved.rtodo', 'tasks.csv', 'tasks.md'):
            path = readback / name
            subprocess.run(['debugfs', '-R', f'dump /home/test/{name} {path}', str(root)],
                           check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            assert path.is_file(), 'Missing persisted guest output: ' + name
            entries.append({'path': name, 'bytes': path.stat().st_size, 'sha256': fltk.sha256(path)})
        subprocess.run([str(output / 'guest-verify'), '--verify-guest', str(readback)], check=True)
        with (readback / 'tasks.csv').open(encoding='utf-8-sig', newline='') as stream:
            rows = list(csv.reader(stream))
        assert len(rows) == 8
        row = next(r for r in rows if r[0] == 'TodoAcceptance')
        assert row[1:] == ['待办', '紧急', '2030-01-02', 'ReliefOS', 'qemu,verified', 'Persisted by real GUI']
        markdown = (readback / 'tasks.md').read_text()
        assert '- [ ] TodoAcceptance' in markdown and 'Persisted by real GUI' in markdown
        for item in metadata['injected_files']:
            if item['guest_path'].endswith('/relieftodo.elf'):
                path = readback / 'relieftodo.elf'
                subprocess.run(['debugfs', '-R', f'dump {item["guest_path"]} {path}', str(root)],
                               check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                assert fltk.sha256(path) == item['sha256']
        (output / 'persisted-verification.json').write_text(json.dumps({'outputs': entries,
            'native_document': True, 'csv': True, 'markdown': True, 'executable_hash': True}, indent=2) + '\n')
    finally:
        root.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--interactive', action='store_true', help='development only; does not certify acceptance')
    args = parser.parse_args()
    output = args.output.resolve()
    metadata = prepare(args.image.resolve(), args.kernel.resolve(), output)
    session = None
    try:
        session = Session(output)
        session.login()
        if args.interactive:
            session.launch()
            code.interact(local={'s': session, 'metadata': metadata, 'output': output,
                                 'time': time, 'fltk': fltk, 'xorg': xorg,
                                 'filename': filename, 'resize': resize, 'menu': menu})
        else:
            exercise(session)
    finally:
        if session:
            session.close()
    assert fltk.sha256(args.image) == metadata['source_image_sha256_before'], 'Original image changed'
    if args.interactive:
        print('Development session finished; full acceptance not certified', flush=True)
    else:
        verify_disk(output, metadata)
        (output / 'acceptance.json').write_text(json.dumps({'passed': True, 'source_image_unchanged': True,
            'platform': 'QEMU KVM / ReliefOS Xorg', 'vmware': 'not verified', 'hardware': 'not verified'}, indent=2) + '\n')
        print('ReliefTodo QEMU GUI and independent disk verification passed', flush=True)


if __name__ == '__main__':
    main()
