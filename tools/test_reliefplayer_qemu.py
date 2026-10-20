#!/usr/bin/env python3
"""Exercise the real FLTK player in a private guest and inspect QEMU audio output."""
from pathlib import Path
import argparse
import array
import io
import json
import math
import os
import re
import struct
import subprocess
import time
import wave
import test_fltk_apps_qemu as fltk
import test_xorg_vm_qemu as xorg
from test_reliefmath_qemu import Session as CommonSession, filename
from test_installer_accounts_qemu import Probe
from qmp_terminal_smoke import text_keys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'out/xorg-vmware-debug'


def prepare(image, kernel, output):
    output.mkdir(parents=True, exist_ok=False)
    fixtures = output / 'fixtures'
    fixtures.mkdir()
    with wave.open(str(fixtures / 'sample.wav'), 'wb') as audio:
        audio.setparams((2, 2, 44100, 0, 'NONE', 'not compressed'))
        block = bytearray()
        for n in range(44100 * 30):
            block += struct.pack('<hh', int(12000 * math.sin(2 * math.pi * 440 * n / 44100)),
                                 int(12000 * math.sin(2 * math.pi * 880 * n / 44100)))
        audio.writeframes(block)
    with wave.open(str(fixtures / 'short.wav'),'wb') as audio:
        audio.setparams((2,2,44100,0,'NONE','not compressed'))
        audio.writeframes(block[:88200])
    subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i',
                    'sine=frequency=660:sample_rate=32000:duration=20',
                    '-c:a', 'libmp3lame', '-q:a', '4', str(fixtures / 'sample.mp3')], check=True)
    (fixtures / 'invalid.wav').write_bytes(b'RIFF\xff\xff\xff\xffWAVEbad')
    probe = fltk.target_compile_probe(output, OUT / 'sysroot/musl', 'clang++', 'ld.lld')
    runner = output / 'runner'
    runner.write_text('''#!/bin/sh
export LC_ALL=zh_CN.UTF-8 LANG=zh_CN.UTF-8
cat /proc/asound/cards >/dev/ttyS0 2>&1
ls -l /dev/dsp >/dev/ttyS0 2>&1
/usr/lib/reliefos/apps/reliefplayer/reliefplayer.elf "$@" >/dev/ttyS0 2>&1 &
pid=$!
echo "RP_START pid=$pid" >/dev/ttyS0
/usr/lib/reliefos/tests/fltk-ui-probe --watch-window-focus "$pid" >/dev/ttyS0 2>&1 &
watcher=$!
wait "$pid"
status=$?
kill "$watcher" 2>/dev/null
echo "RP_EXIT status=$status" >/dev/ttyS0
''')
    payload = [(OUT / 'userland/reliefplayer.elf', '/usr/lib/reliefos/apps/reliefplayer/reliefplayer.elf', 0o100755),
               (ROOT / 'resources/build-art/app-icons/reliefplayer.bmp', '/usr/lib/reliefos/apps/reliefplayer/reliefplayer.bmp', 0o100644),
               (ROOT / 'userland/apps/reliefplayer/reliefplayer.desktop', '/usr/share/applications/reliefos-reliefplayer.desktop', 0o100644),
               (OUT / 'generated/nls/zh_CN/LC_MESSAGES/leonos.mo', fltk.LOCALE_GUEST, 0o100644),
               (probe, fltk.PROBE_GUEST, 0o100755), (runner, '/usr/bin/rptest', 0o100755)]
    for name in ('sample.wav', 'sample.mp3', 'short.wav', 'invalid.wav'):
        payload.append((fixtures / name, '/home/test/' + name, 0o100644))
    fltk.validate_target_elf(OUT / 'userland/reliefplayer.elf', sysroot=OUT / 'sysroot/musl', required_needed=('libfltk.so.1.3',))
    metadata = fltk.prepare_disk(image, kernel, output, payload)
    (output / 'prepare.json').write_text(json.dumps(metadata, indent=2) + '\n')
    return metadata


class Session(CommonSession):
    def __init__(self, output):
        self.output = output
        self.serial, self.qmp = output / 'serial.log', output / 'qmp.sock'
        command = ['qemu-system-x86_64', '-enable-kvm', '-cpu', 'host', '-machine', 'q35',
                   '-m', '4096', '-smp', '2', '-bios', '/usr/share/edk2/x64/OVMF.4m.fd',
                   '-display', 'none', '-serial', f'file:{self.serial}', '-vga', 'vmware',
                   '-device', 'qemu-xhci', '-device', 'usb-tablet', '-netdev', 'user,id=net0',
                   '-device', 'e1000,netdev=net0', '-drive', f'file={output}/guest-private.raw,format=raw,if=ide',
                   '-qmp', f'unix:{self.qmp},server=on,wait=off', '-no-reboot', '-no-shutdown',
                   '-audiodev', f'wav,id=audio0,path={output}/output.wav,out.frequency=48000,out.channels=2,out.format=s16',
                   '-device', 'intel-hda,msi=off', '-device', 'hda-duplex,audiodev=audio0']
        (output / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
        self.log = (output / 'qemu.log').open('w')
        self.process = subprocess.Popen(command, cwd=ROOT, stdout=self.log, stderr=self.log, start_new_session=True)
        deadline = time.monotonic() + 15
        while not self.qmp.exists():
            if self.process.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError('QEMU startup failed')
            time.sleep(.1)
        self.probe = Probe(self.qmp, output)
        def key(value):
            codes = value.split('-')
            events = [{'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': item}}}
                      for down, seq in ((True, codes), (False, reversed(codes))) for item in seq]
            self.probe.command('input-send-event', {'events': events})
            time.sleep(.16)
        def text(value):
            for char in value:
                key(text_keys(char)[0])
            time.sleep(.4)
        self.probe.key, self.probe.text = key, text

    def launch(self, path=''):
        self.launch_offset = len(self.serial.read_text(errors='replace'))
        self.probe.click(300, 200)
        self.probe.key('ctrl-u')
        self.probe.text('rptest' + (' ' + path if path else ''))
        self.probe.key('ret')
        self.wait('[reliefplayer.elf] FLTK audio player ready', self.launch_offset)
        log = self.serial.read_text(errors='replace')[self.launch_offset:]
        self.pid = int(re.search(r'RP_START pid=(\d+)', log)[1])
        self.geometry = fltk.wait_client_geometry(self.serial, self.process, self.pid, self.launch_offset)

    def wait(self, marker, offset=0):
        xorg.wait_serial_after(self.serial, marker, self.process, offset, timeout=45)

    def open_audio(self, name):
        self.probe.key('ctrl-o')
        filename(self, '/home/test/' + name, 'audio-chooser')

    def close_app(self):
        offset = len(self.serial.read_text(errors='replace'))
        x, y, width, _ = self.geometry
        self.probe.click(x + width - 10, y - 11)
        self.wait('RP_EXIT status=0', offset)


def exercise(s):
    s.launch()
    s.screenshot('reliefplayer-empty')
    offset = len(s.serial.read_text(errors='replace'))
    s.open_audio('sample.wav')
    s.wait('state=playing', offset)
    time.sleep(3)
    s.screenshot('reliefplayer-wav')
    y = s.geometry[3] - 190
    offset = len(s.serial.read_text(errors='replace'))
    s.click(70, y + 82)
    s.wait('state=paused', offset)
    s.screenshot('reliefplayer-paused')
    s.click(s.geometry[2] // 2, y + 40)
    s.wait('seek=', offset)
    s.wait_text(r'00:1[45]', 'seek-paused')
    offset = len(s.serial.read_text(errors='replace'))
    s.probe.key('spc')
    s.wait('state=playing', offset)
    time.sleep(2)
    offset = len(s.serial.read_text(errors='replace'))
    s.click(s.geometry[2] - 90, y + 133)
    s.wait('volume=0', offset)
    time.sleep(2)
    s.click(s.geometry[2] - 90, y + 133)
    s.wait('volume=70', offset)
    offset = len(s.serial.read_text(errors='replace'))
    s.open_audio('sample.mp3')
    s.wait('state=playing', offset)
    time.sleep(4)
    s.wait_text('单声道','mp3-format')
    s.screenshot('reliefplayer-mp3')
    offset = len(s.serial.read_text(errors='replace'))
    s.click(180, y + 82)
    s.wait('state=stopped position=0', offset)
    s.screenshot('reliefplayer-stopped')
    s.probe.key('spc')
    s.wait('state=playing', offset)
    time.sleep(2)
    offset=len(s.serial.read_text(errors='replace'))
    s.click(400,y+82)
    s.wait('seek=',offset)
    time.sleep(.5)
    offset=len(s.serial.read_text(errors='replace'))
    s.click(400,y+82)
    s.wait('state=ended',offset)
    s.probe.key('spc')
    s.wait('state=playing',offset)
    s.close_app()
    s.launch('/home/test/short.wav')
    s.wait('state=ended',s.launch_offset)
    s.screenshot('reliefplayer-complete')
    s.close_app()
    s.launch('/home/test/invalid.wav')
    s.wait('state=failed', s.launch_offset)
    s.wait_text('音频文件无效或不完整', 'invalid-audio')
    s.screenshot('reliefplayer-invalid')
    s.close_app()


def inspect_audio(path):
    raw=path.read_bytes()
    finalized=True
    if (len(raw)>44 and raw[:4]==b'RIFF' and raw[8:16]==b'WAVEfmt ' and
            raw[36:40]==b'data' and struct.unpack_from('<I',raw,4)[0] in (0,36) and raw[40:44]==b'\0'*4):
        # Preserve original QEMU bytes; repair only its unfinalized RIFF sizes
        # in a separate file, never the recorded PCM.
        assert struct.unpack_from('<IHHIIHH',raw,16)==(16,1,2,48000,192000,4,16)
        assert (len(raw)-44)%4==0
        fixed=bytearray(raw)
        struct.pack_into('<I',fixed,4,len(fixed)-8)
        struct.pack_into('<I',fixed,40,len(fixed)-44)
        (path.parent/'output-normalized.wav').write_bytes(fixed)
        raw=bytes(fixed); finalized=False
    with wave.open(io.BytesIO(raw), 'rb') as audio:
        assert (audio.getframerate(), audio.getnchannels(), audio.getsampwidth()) == (48000, 2, 2)
        samples = array.array('h', audio.readframes(audio.getnframes()))
    if os.sys.byteorder != 'little':
        samples.byteswap()
    size = 4096
    tables = {f: [(math.cos(2 * math.pi * f * n / 48000), math.sin(2 * math.pi * f * n / 48000))
                  for n in range(size)] for f in (440, 660, 880)}
    wav_windows = mp3_windows = 0
    def power(values, frequency):
        table = tables[frequency]
        real = sum(value * table[n][0] for n, value in enumerate(values))
        imag = sum(value * table[n][1] for n, value in enumerate(values))
        return real * real + imag * imag
    for at in range(0, len(samples) - size * 2, size * 2):
        left, right = samples[at:at + size * 2:2], samples[at + 1:at + size * 2:2]
        if max(map(abs, left)) < 100:
            continue
        a, b, c = (power(left, f) for f in (440, 660, 880))
        if a > 20 * (b + c) and power(right, 880) > 20 * power(right, 440):
            wav_windows += 1
        if b > 20 * (a + c) and power(right, 660) > 20 * power(right, 440):
            mp3_windows += 1
    assert wav_windows >= 12 and mp3_windows >= 12, (wav_windows, mp3_windows)
    return {'rate': 48000, 'channels': 2, 'qemu_header_finalized':finalized,'wav_stereo_tone_windows': wav_windows,
            'mp3_resampled_mono_tone_windows': mp3_windows, 'window_frames': size}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--kernel', type=Path, default=fltk.DEFAULT_KERNEL)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    if output == ROOT / 'out' or not output.is_relative_to(ROOT / 'out'):
        parser.error('output must be a fresh repository out subdirectory')
    metadata = prepare(args.image.resolve(), args.kernel.resolve(), output)
    session = None
    try:
        session = Session(output)
        session.login()
        print('QEMU desktop login complete', flush=True)
        exercise(session)
    except Exception:
        if session:
            session.frame('verification-failure')
        raise
    finally:
        if session:
            session.close()
    audio = inspect_audio(output / 'output.wav')
    serial = (output / 'serial.log').read_text(errors='replace')
    assert serial.count('RP_EXIT status=0') == 3
    assert '[reliefplayer.elf] recover count=' not in serial, 'audio underrun during normal playback'
    assert 'rate=44100 channels=2' in serial and 'rate=32000 channels=1' in serial
    assert fltk.sha256(args.image) == metadata['source_image_sha256_before']
    result = {'passed': True, 'platform': 'QEMU KVM / ReliefOS Xorg / HDA', 'audio': audio,
              'source_image_unchanged': True, 'vmware_verified': False, 'default_vmdk_boot_verified': False}
    (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2), flush=True)


if __name__ == '__main__':
    main()
