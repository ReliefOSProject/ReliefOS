#!/usr/bin/env python3
"""Exercise the real IceWM shortcut and screenshot editor in a private guest."""
from pathlib import Path
import argparse
import json
import code
import traceback
import re
import subprocess
import time
from PIL import Image, ImageChops
import test_fltk_apps_qemu as fltk
import test_xorg_vm_qemu as xorg
from test_reliefmath_qemu import Session, dialog_origin, wait_focus
from test_reliefdiagram_qemu import explicit_keys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'out/xorg-vmware-debug'


def prepare(image, kernel, output, build_dir=OUT):
    output.mkdir(parents=True, exist_ok=False)
    probe = fltk.target_compile_probe(output, build_dir / 'sysroot/musl', 'clang++', 'ld.lld',
                                      build_dir=build_dir)
    template = json.loads((output / 'probe-build.command.json').read_text())
    commands = {key: list(value) for key, value in template.items()}
    source = ROOT / 'tools/tests/reliefcapture_clipboard.cpp'
    commands['compile'][-3] = str(source)
    commands['compile'][-1] = str(output / 'clipboard.o')
    commands['link'][commands['link'].index(str(output / 'fltk_ui_probe.o'))] = str(output / 'clipboard.o')
    commands['link'][commands['link'].index(str(probe))] = str(output / 'clipboard.elf')
    for command in commands.values():
        subprocess.run(command, cwd=ROOT, check=True, capture_output=True)
    runner = output / 'runner'
    runner.write_text('''#!/bin/sh
export LC_ALL=zh_CN.UTF-8 LANG=zh_CN.UTF-8
if [ ! -f /home/test/capture-clipboard.pid ]; then
 /usr/lib/reliefos/tests/reliefcapture-clipboard >/dev/ttyS0 2>&1 &
 echo $! >/home/test/capture-clipboard.pid
fi
/usr/lib/reliefos/apps/reliefcapture/reliefcapture.elf "$@" >/dev/ttyS0 2>&1 &
pid=$!
echo "RC_START pid=$pid args=$*" >/dev/ttyS0
(
 while kill -0 "$pid" 2>/dev/null; do
  /usr/lib/reliefos/tests/fltk-ui-probe --watch-window-focus "$pid" >/dev/ttyS0 2>&1
  sleep 1
 done
) &
watcher=$!
wait "$pid"
status=$?
kill "$watcher" 2>/dev/null
echo "RC_EXIT status=$status" >/dev/ttyS0
''')
    elf = build_dir / 'userland/reliefcapture.elf'
    fltk.validate_target_elf(elf, sysroot=build_dir / 'sysroot/musl', required_needed=('libfltk.so.1.3', 'libX11.so.6'))
    payload = [(elf, '/usr/lib/reliefos/apps/reliefcapture/reliefcapture.elf', 0o100755),
               (ROOT / 'resources/build-art/app-icons/reliefcapture.bmp', '/usr/lib/reliefos/apps/reliefcapture/reliefcapture.bmp', 0o100644),
               (ROOT / 'userland/apps/reliefcapture/reliefcapture.desktop', '/usr/share/applications/reliefos-reliefcapture.desktop', 0o100644),
               (build_dir / 'generated/nls/zh_CN/LC_MESSAGES/leonos.mo', fltk.LOCALE_GUEST, 0o100644),
               (build_dir / 'upstream/x11-development/usr/lib/libX11.so', '/usr/lib/libX11.so.6', 0o100755),
               (ROOT / 'system/xorg/icewm/keys', '/etc/reliefos/icewm/keys', 0o100644),
               (runner, '/usr/bin/reliefcapture', 0o100755), (probe, fltk.PROBE_GUEST, 0o100755),
               (output / 'clipboard.elf', '/usr/lib/reliefos/tests/reliefcapture-clipboard', 0o100755)]
    metadata = fltk.prepare_disk(image, kernel, output, payload)
    (output / 'prepare.json').write_text(json.dumps(metadata, indent=2) + '\n')
    return metadata


def wait(s, marker, offset=0, timeout=60):
    return xorg.wait_serial_after(s.serial, marker, s.process, offset, timeout=timeout)


def shortcut(s):
    offset = len(s.serial.read_text(errors='replace'))
    s.probe.key('meta_l-shift-s')
    wait(s, '[reliefcapture.elf] selection-ready', offset)
    return offset


def capture_done(s, offset):
    wait(s, '[reliefcapture.elf] copied size=', offset)
    log = s.serial.read_text(errors='replace')
    if not getattr(s, 'pid', None):
        s.pid = int(re.findall(r'RC_START pid=(\d+)', log)[0])
    s.launch_offset = 0
    s.geometry = fltk.wait_client_geometry(s.serial, s.process, s.pid, offset)
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        frame = s.frame('capture-editor-paint')
        x, y, w, _ = s.geometry
        samples = [frame.getpixel((x + dx, y + 5))[:3] for dx in (5, w // 2, w - 10)]
        if all(170 <= r <= 230 and abs(r - g) < 8 and abs(r - b) < 8 for r, g, b in samples):
            break
    else:
        raise AssertionError('The editor client area was not painted')
    size = re.findall(r'captured size=(\d+)x(\d+)', log[offset:])[-1]
    return tuple(map(int, size))


def drag(s, start, end, screenshot=None):
    s.probe.move(*start)
    s.probe.command('input-send-event', {'events': [{'type': 'btn', 'data': {'down': True, 'button': 'left'}}]})
    for n in range(1, 9):
        s.probe.move(round(start[0] + (end[0] - start[0]) * n / 8),
                     round(start[1] + (end[1] - start[1]) * n / 8))
        time.sleep(.06)
    if screenshot:
        time.sleep(.5)
        s.screenshot(screenshot)
    s.probe.command('input-send-event', {'events': [{'type': 'btn', 'data': {'down': False, 'button': 'left'}}]})
    time.sleep(.4)


def save(s, name):
    s.click(s.geometry[2] - 40, s.geometry[3] - 22)
    wait_focus(s, True)
    offset = len(s.serial.read_text(errors='replace'))
    s.probe.key('ctrl-s')
    x, y = save_dialog(s)
    s.probe.click(x + 300, y + 322)
    s.fill(name)
    s.probe.key('ret')
    wait(s, '[reliefcapture.elf] saved PNG', offset)


def save_dialog(s):
    wait_focus(s, False)
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        frame = s.frame('capture-save')
        try:
            return dialog_origin(frame, 500, 386)
        except AssertionError:
            continue
    raise AssertionError('The screenshot save dialog did not paint')


def choice(s, x, y, value):
    state = getattr(s, 'choices', {})
    current = state.get((x, y), 1 if x == 70 else 0)
    if value == current:
        return
    s.click(x, y)
    fields = {70: (12, 126), 200: (142, 122), 330: (278, 110)}
    field, width = fields[x]
    deadline = time.monotonic() + 12
    while time.monotonic() < deadline:
        frame = s.frame('capture-choice')
        gx, gy = s.geometry[:2]
        # The default native gtk+ popup centers its selected row over the
        # choice field. Locate that real blue highlight instead of relying
        # on Home (unsupported here) or OCR of the small Chinese glyphs.
        column = gx + field + width - 8
        runs = []
        start = None
        for row in range(max(0, gy + y - 100), min(frame.height, gy + y + 110)):
            r, g, b = frame.getpixel((column, row))[:3]
            highlighted = r < 40 and g < 40 and b > 85
            if highlighted and start is None:
                start = row
            elif not highlighted and start is not None:
                if 12 <= row - start <= 30:
                    runs.append((start, row))
                start = None
        runs = [run for run in runs if abs((run[0] + run[1]) / 2 - gy - y) <= 8]
        if runs:
            top, bottom = min(runs, key=lambda run: abs((run[0] + run[1]) / 2 - gy - y))
            pitch = bottom - top + 2
            s.probe.click(gx + field + 20, (top + bottom - 1) // 2 + (value - current) * pitch)
            break
    else:
        raise AssertionError('Native choice popup selection did not paint')
    state[(x, y)] = value
    s.choices = state
    time.sleep(.4)


def canvas(s, size):
    # Independently calculated widget geometry; capture source dimensions are
    # obtained from the actual result, including tablet quantization.
    w, h = s.geometry[2:]
    aw, ah = w - 76, h - 214
    iw, ih = size
    scale = min(1, aw / iw, ah / ih)
    dw, dh = int(iw * scale), int(ih * scale)
    return (s.geometry[0] + 30 + (aw - dw) // 2,
            s.geometry[1] + 138 + (ah - dh) // 2, dw, dh)


def desktop_settled(s):
    wait(s, 'name=fastfetch code=0', timeout=30)
    previous = None
    stable = None
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        frame = s.frame('reliefcapture-desktop-settled')
        # Fixed fixture terminal; exclude its blinking prompt and taskbar clock.
        current = frame.crop((8, 26, 712, 290)).convert('RGB')
        blue = sum(b > 170 and b > r + 40 and b > g + 20 for r, g, b in current.getdata())
        if blue > 200 and previous is not None and ImageChops.difference(previous, current).getbbox() is None:
            if stable is None:
                stable = time.monotonic()
            if time.monotonic() - stable > .6:
                return
        else:
            stable = None
        previous = current
        time.sleep(.3)
    raise AssertionError('The fixture desktop did not finish painting')


def exercise(s):
    s.probe.move(5, 110)
    # The fixture starts fastfetch asynchronously after mapping its terminal.
    # Wait for its final text before taking an independent desktop reference.
    desktop_settled(s)
    s.frame('reliefcapture-desktop-before')
    offset = shortcut(s)
    s.screenshot('reliefcapture-selection')
    drag(s, (120, 190), (760, 550), 'reliefcapture-region-selection')
    size = capture_done(s, offset)
    assert abs(size[0] - 640) <= 2 and abs(size[1] - 360) <= 2, size
    s.capture_size = size
    s.screenshot('reliefcapture-editor')
    save(s, 'capture-region.png')
    x, y, w, h = canvas(s, size)
    offset = len(s.serial.read_text(errors='replace'))
    drag(s, (x + 70, y + 75), (x + 230, y + 145))
    wait(s, 'annotation count=1', offset)
    s.probe.key('ctrl-z')
    wait(s, 'history count=0', offset)
    s.screenshot('reliefcapture-undo')
    s.probe.key('ctrl-y')
    wait(s, 'history count=1', offset)
    choice(s, 70, 93, 2)
    drag(s, (x + 290, y + 80), (x + 485, y + 210))
    wait(s, 'annotation count=2', offset)
    choice(s, 70, 93, 0)
    drag(s, (x + 80, y + 245), (x + 250, y + 290))
    wait(s, 'annotation count=3', offset)
    choice(s, 70, 93, 3)
    s.probe.click(x + 315, y + 250)
    s.probe.text('ReliefOS')
    s.probe.key('ret')
    wait(s, 'annotation count=4', offset)
    s.screenshot('reliefcapture-annotated')
    save(s, 'capture-annotated.png')
    offset = len(s.serial.read_text(errors='replace'))
    s.probe.key('ctrl-c')
    wait(s, 'RC_CLIPBOARD_BMP', offset)
    # A repeated shortcut while Save As is open waits until that dialog exits.
    offset = len(s.serial.read_text(errors='replace'))
    s.probe.key('ctrl-s')
    save_dialog(s)
    s.probe.key('meta_l-shift-s')
    wait(s, '[reliefcapture.elf] capture-queued', offset)
    assert 'capture-reading' not in s.serial.read_text(errors='replace')[offset:]
    s.probe.key('esc')
    wait(s, '[reliefcapture.elf] selection-ready', offset)
    s.probe.key('esc')
    wait(s, 'capture-canceled', offset)
    wait_focus(s, True)
    # Repeat hotkey is routed to the same long-lived editor.
    offset = shortcut(s)
    s.probe.key('esc')
    wait(s, 'capture-canceled', offset)
    wait_focus(s, True)
    s.screenshot('reliefcapture-canceled')
    assert s.serial.read_text(errors='replace').count('FLTK screen capture ready') == 1
    # Window and full-screen capture use the frozen desktop beneath the editor.
    offset = shortcut(s)
    s.probe.key('w')
    s.probe.move(300, 300)
    s.screenshot('reliefcapture-window-selection')
    s.probe.click(300, 300)
    window_size = capture_done(s, offset)
    s.window_size = window_size
    assert window_size[0] > 300 and window_size[1] > 150, window_size
    save(s, 'capture-window.png')
    offset = shortcut(s)
    s.probe.key('f')
    screen_size = capture_done(s, offset)
    s.screen_size = screen_size
    save(s, 'capture-full.png')
    choice(s, 200, 36, 2)
    choice(s, 330, 36, 1)
    offset = len(s.serial.read_text(errors='replace'))
    s.probe.key('ctrl-n')
    time.sleep(.5)
    s.screenshot('reliefcapture-countdown')
    assert 'capture-reading' not in s.serial.read_text(errors='replace')[offset:]
    s.probe.key('esc')
    wait(s, 'capture-canceled', offset)
    wait_focus(s, True)
    offset = len(s.serial.read_text(errors='replace'))
    s.probe.key('ctrl-n')
    assert capture_done(s, offset) == screen_size
    save(s, 'capture-delayed.png')
    # Escape queued for the editor must survive hiding its native window.
    choice(s, 330, 36, 0)
    wait_focus(s, True)
    offset = len(s.serial.read_text(errors='replace'))
    keys = [('ctrl', True), ('n', True), ('n', False), ('ctrl', False),
            ('esc', True), ('esc', False)]
    s.probe.command('input-send-event', {'events': [
        {'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': key}}}
        for key, down in keys]})
    wait(s, 'capture-canceled', offset)
    wait_focus(s, True)
    assert 'captured size=' not in s.serial.read_text(errors='replace')[offset:]
    s.screenshot('reliefcapture-fullscreen-editor')
    offset = len(s.serial.read_text(errors='replace'))
    s.probe.key('meta_l-q')
    wait(s, 'RC_EXIT status=0', offset)
    # Wait for IceWM to restore focus after closing the client window.
    s.probe.click(100, 200)
    time.sleep(1)
    offset = len(s.serial.read_text(errors='replace'))
    s.probe.key('meta_l-shift-s')
    wait(s, '[reliefcapture.elf] capture-reading', offset)
    s.probe.key('esc')
    wait(s, 'RC_EXIT status=0', offset)
    s.window_size, s.screen_size = window_size, screen_size
    # Flush the actual guest disk before QMP terminates this test VM.
    s.probe.click(300, 200)
    s.probe.text('sync')
    s.probe.key('ret')
    time.sleep(5)


def verify(output, metadata, sizes):
    folder = output / 'readback'
    folder.mkdir(exist_ok=True)
    part = next(p for p in metadata['gpt_partitions'] if p['name'] == 'RELIEFOS_ROOT')
    root = folder / 'root.ext4'
    subprocess.run(['dd', 'if=' + str(output / 'guest-private.raw'), 'of=' + str(root),
                    'bs=512', 'skip=' + str(part['first']),
                    'count=' + str(int(part['last']) - int(part['first']) + 1),
                    'conv=sparse', 'status=none'], check=True)
    names = ['capture-region.png', 'capture-annotated.png', 'capture-window.png', 'capture-full.png', 'capture-delayed.png']
    listing = subprocess.check_output(['debugfs', '-R', 'ls -p /home/test', str(root)], stderr=subprocess.DEVNULL, text=True)
    guest_files = re.findall(r'^/\d+/[^/]+/\d+/\d+/([^/]+)/', listing, re.M)
    guest_paths = {}
    data = {}
    for name in names:
        # A native filename-field selection can retain part of its suggested
        # basename. Read the unique real export and preserve its actual path
        # in the report; never rename or rewrite the guest's saved image.
        candidates = [name] if name in guest_files else [item for item in guest_files if item.endswith(name)]
        assert len(candidates) == 1, (name, candidates)
        actual = candidates[0]
        guest_paths[name] = '/home/test/' + actual
        subprocess.run(['debugfs', '-R', f'dump {fltk.debugfs_quote(guest_paths[name])} {fltk.debugfs_quote(folder / name)}', str(root)], check=True, capture_output=True)
        image = Image.open(folder / name).convert('RGB')
        image.load()
        data[name] = image
    assert data[names[0]].size == data[names[1]].size == sizes[0]
    assert data[names[2]].size == sizes[1]
    assert data[names[3]].size == data[names[4]].size == sizes[2]
    base, annotated = data[names[0]], data[names[1]]
    desktop = Image.open(output / 'reliefcapture-desktop-before.png').convert('RGB')
    source_mismatch = 1.0
    for dx in range(-2, 3):
        for dy in range(-2, 3):
            expected = desktop.crop((120 + dx, 190 + dy,
                                     120 + dx + base.width, 190 + dy + base.height))
            differences = ImageChops.difference(expected, base)
            mismatch = sum(max(pixel) > 8 for pixel in differences.getdata()) / (base.width * base.height)
            source_mismatch = min(source_mismatch, mismatch)
    assert source_mismatch < .005, source_mismatch
    delta = ImageChops.difference(base, annotated)
    changed = sum(max(p) > 24 for p in delta.getdata())
    assert changed > 1000, changed
    red = sum(r > 160 and g < 110 and b < 110 for r, g, b in annotated.getdata())
    assert red > 1500, red
    # Clipboard bitmaps are read by a separate X11 client, including INCR.
    clipboard_matches = 0
    for name in re.findall(r'/(capture-clipboard-\d+\.bmp)/', listing):
        subprocess.run(['debugfs', '-R', f'dump /home/test/{name} {fltk.debugfs_quote(folder / name)}', str(root)], check=True, capture_output=True)
        try:
            clipboard = Image.open(folder / name).convert('RGB')
            if clipboard.size == annotated.size and ImageChops.difference(clipboard, annotated).getbbox() is None:
                clipboard_matches += 1
        except (OSError, ValueError):
            continue
    assert clipboard_matches, 'The copied annotated image did not match its PNG export'
    assert fltk.sha256(Path(metadata['source_image'])) == metadata['source_image_sha256_before']
    result = {'passed': True, 'platform': 'QEMU KVM / ReliefOS Xorg / IceWM',
              'source_image_unchanged': True, 'region_size': sizes[0], 'window_size': sizes[1],
              'screen_size': sizes[2], 'changed_annotation_pixels': changed, 'red_annotation_pixels': red,
              'source_pixel_mismatch_fraction': source_mismatch,
              'independent_clipboard_matches': clipboard_matches, 'vmware_verified': False,
              'default_vmdk_boot_verified': False,
              'guest_png_paths': guest_paths,
              'readbacks': {name: fltk.sha256(folder / name) for name in names}}
    (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--build-dir', type=Path, default=OUT)
    parser.add_argument('--kernel', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--interactive', action='store_true', help='keep this owned VM available for debugging a failed assertion')
    args = parser.parse_args()
    output = args.output.resolve()
    if output == ROOT / 'out' or not output.is_relative_to(ROOT / 'out'):
        parser.error('output must be a fresh repository out subdirectory')
    build_dir = args.build_dir.resolve()
    kernel = args.kernel.resolve() if args.kernel else build_dir / 'generated/system/kernel.sys'
    metadata = prepare(args.image.resolve(), kernel, output, build_dir)
    s = None
    try:
        s = Session(output)
        explicit_keys(s)
        s.login()
        print('QEMU desktop login complete', flush=True)
        exercise(s)
        sizes = s.capture_size, s.window_size, s.screen_size
    except Exception:
        if s:
            s.frame('verification-failure')
        if args.interactive:
            traceback.print_exc()
            code.interact(local=locals())
            sizes = s.capture_size, s.window_size, s.screen_size
        else:
            raise
    finally:
        if s:
            s.close()
    verify(output, metadata, sizes)


if __name__ == '__main__':
    main()
