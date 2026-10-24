#!/usr/bin/env python3
"""ReliefMath++ acceptance using a hash-checked private QEMU disk."""
from pathlib import Path
import argparse
import code
import json
import os
import re
import subprocess
import time
import test_fltk_apps_qemu as fltk
import test_vdso_qemu as vdso
import test_xorg_vm_qemu as xorg
from test_reliefmath_qemu import Session as MathSession, filename, resize, wait_focus, dialog_origin
from qmp_terminal_smoke import text_keys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'out/xorg-vmware-debug'


def prepare(image, kernel, output):
    output.mkdir(parents=True, exist_ok=False)
    probe = fltk.target_compile_probe(output, OUT / 'sysroot/musl', 'clang++', 'ld.lld')
    runner = output / 'rmp-runner'
    runner.write_text('''#!/bin/sh
export LC_ALL=zh_CN.UTF-8 LANG=zh_CN.UTF-8
/usr/lib/reliefos/apps/reliefmathpp/reliefmathpp.elf "$@" >/dev/ttyS0 2>&1 &
pid=$!
echo "RMP_START pid=$pid" >/dev/ttyS0
/usr/lib/reliefos/tests/fltk-ui-probe --watch-window-focus "$pid" >/dev/ttyS0 2>&1 &
watch=$!
wait "$pid"
status=$?
kill "$watch" 2>/dev/null
echo "RMP_EXIT status=$status" >/dev/ttyS0
''')
    entry = output / 'reliefmathpp-entry'
    entry.write_text('#!/bin/sh\nexec /usr/lib/reliefos/apps/reliefmathpp/reliefmathpp.elf "$@"\n')
    payload = [(OUT / 'userland/reliefmathpp.elf', '/usr/lib/reliefos/apps/reliefmathpp/reliefmathpp.elf', 0o100755),
               (OUT / 'userland/fileman.elf', '/usr/lib/reliefos/apps/fileman/fileman.elf', 0o100755),
               (OUT / 'generated/nls/zh_CN/LC_MESSAGES/leonos.mo', fltk.LOCALE_GUEST, 0o100644),
               (ROOT / 'userland/apps/reliefmathpp/reliefmathpp.desktop', '/usr/share/applications/reliefos-reliefmathpp.desktop', 0o100644),
               (ROOT / 'system/rootfs/usr/share/mime/packages/reliefmathpp.xml', '/usr/share/mime/packages/reliefmathpp.xml', 0o100644),
               (ROOT / 'resources/build-art/app-icons/reliefmathpp.bmp', '/usr/lib/reliefos/apps/reliefmathpp/reliefmathpp.bmp', 0o100644),
               (entry, '/usr/bin/reliefmathpp', 0o100755), (runner, '/usr/bin/rmp', 0o100755),
               (probe, fltk.PROBE_GUEST, 0o100755)]
    for size in (16, 32, 48, 64, 128, 256):
        path = ROOT / f'resources/build-art/app-icons/hicolor/{size}x{size}/apps/reliefos-reliefmathpp.png'
        payload.append((path, f'/usr/share/icons/hicolor/{size}x{size}/apps/{path.name}', 0o100644))
    fltk.validate_target_elf(OUT / 'userland/reliefmathpp.elf', sysroot=OUT / 'sysroot/musl', required_needed=('libfltk.so.1.3',))
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
    (output / 'prepare.json').write_text(json.dumps(metadata, indent=2) + '\n')
    return metadata


class Session(MathSession):
    def __init__(self, output):
        super().__init__(output)
        def key(value):
            codes = value.split('-')
            events = [{'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': item}}}
                      for down, sequence in ((True, codes), (False, reversed(codes))) for item in sequence]
            self.probe.command('input-send-event', {'events': events})
            time.sleep(.2)
        def text(value):
            for ch in value:
                punctuation = {'^':'shift-6', ',':'comma', '(':'shift-9', ')':'shift-0', '*':'shift-8', '+':'shift-equal', '<':'shift-comma', '>':'shift-dot'}
                key(punctuation.get(ch,text_keys(ch)[0]))
            time.sleep(.4)
        self.probe.key = key
        self.probe.text = text

    def launch(self, path=''):
        self.launch_offset = len(self.serial.read_text(errors='replace'))
        self.probe.click(300, 200)
        self.probe.text('rmp' + (' ' + path if path else '')); self.keys('ret')
        self.wait_log('[reliefmathpp.elf] FLTK geometry workspace ready', self.launch_offset)
        log = self.serial.read_text(errors='replace')[self.launch_offset:]
        self.pid = int(re.search(r'RMP_START pid=(\d+)', log)[1])
        self.geometry = fltk.wait_client_geometry(self.serial, self.process, self.pid, self.launch_offset)
        self.wait_log('[reliefmathpp.elf] sampling complete', self.launch_offset)
        self.screenshot('reliefmathpp-startup')
        return self.geometry

    def wait_log(self, text, offset=0, timeout=90):
        xorg.wait_serial_after(self.serial, text, self.process, offset, timeout=timeout)

    def save(self, path=None):
        offset = len(self.serial.read_text(errors='replace'))
        self.keys('ctrl-s')
        if path:
            filename(self, path, 'save-file')
        self.wait_log('[reliefmathpp.elf] saved objects=', offset)

    def close_app(self):
        offset = len(self.serial.read_text(errors='replace'))
        x, y, w, h = self.geometry
        self.probe.click(x+w-10, y-11)
        self.wait_log('RMP_EXIT status=0', offset)


def exercise(s):
    s.launch(); s.screenshot('reliefmathpp-2d')
    offset = len(s.serial.read_text(errors='replace'))
    s.click(75, 112); s.click(170, 376); s.fill('QemuPlot')
    s.click(190, 466); s.fill('x^2/4'); s.keys('ctrl-ret')
    s.wait_log('[reliefmathpp.elf] sampling complete', offset)
    s.screenshot('reliefmathpp-function-edit')
    s.keys('ctrl-d', 'ctrl-z', 'ctrl-y')
    offset=len(s.serial.read_text(errors='replace'));s.keys('ctrl-z')
    s.wait_log('[reliefmathpp.elf] sampling complete',offset)
    s.save('/home/test/verified.rmathpp')
    offset = len(s.serial.read_text(errors='replace'))
    s.click(607, 49); filename(s, '/home/test/plot2d.svg', 'svg-file')
    s.wait_log('[reliefmathpp.elf] export complete', offset)
    offset = len(s.serial.read_text(errors='replace'))
    s.click(710, 49); filename(s, '/home/test/plot2d.png', 'png-file')
    s.wait_log('[reliefmathpp.elf] export complete', offset)
    s.click(150, 82)
    offset = len(s.serial.read_text(errors='replace'))
    s.click(75, 112); s.wait_log('[reliefmathpp.elf] sampling complete', offset)
    s.screenshot('reliefmathpp-3d')
    x,y,w,h = s.geometry
    s.probe.move(x+850,y+400);s.probe.button(True);s.probe.move(x+925,y+438);s.probe.button(False)
    s.screenshot('reliefmathpp-3d-rotated'); s.save()
    s.click(200, 82);s.click(76, 112);wait_focus(s, False)
    dx,dy = dialog_origin(s.frame('calculator'),740,522)
    s.probe.click(dx+300,dy+60);s.keys('home','down','down','down','ret')
    s.probe.click(dx+280,dy+98);s.fill('x^2')
    s.probe.click(dx+610,dy+176);s.fill('3')
    offset = len(s.serial.read_text(errors='replace'));s.probe.click(dx+180,dy+223)
    s.wait_log('[reliefmathpp.elf] calculation mode=3 value=9',offset)
    s.screenshot('reliefmathpp-calculation');s.probe.click(dx+650,dy+488);wait_focus(s,True)
    s.click(620,140);wait_focus(s,False)
    dx,dy=dialog_origin(s.frame('surface-calculator'),760,560)
    s.probe.click(dx+300,dy+60);s.keys('home','down','ret')
    s.probe.click(dx+310,dy+100);s.fill('x^2+y^2')
    s.probe.click(dx+200,dy+140);s.fill('2')
    s.probe.click(dx+540,dy+140);s.fill('3')
    offset=len(s.serial.read_text(errors='replace'));s.probe.click(dx+170,dy+229)
    s.wait_log('[reliefmathpp.elf] calculation mode=11 value=13',offset)
    s.screenshot('reliefmathpp-surface-calculation');s.probe.click(dx+670,dy+525);wait_focus(s,True)
    resize(s,1000,680);s.screenshot('reliefmathpp-minimum-size');s.save();s.close_app()
    s.launch('/home/test/verified.rmathpp');s.wait_log('[reliefmathpp.elf] opened objects=5',s.launch_offset)
    s.screenshot('reliefmathpp-reopened');s.close_app()


def verify_disk(output, metadata):
    partition = next(p for p in metadata['gpt_partitions'] if p['name']=='RELIEFOS_ROOT')
    root=output/'verified-root.ext4'
    subprocess.run(['dd','if='+metadata['private_image'],'of='+str(root),'bs=512',
                    'skip='+str(partition['first']),'count='+str(partition['last']-partition['first']+1),
                    'conv=sparse','status=none'],check=True)
    folder=output/'readback';folder.mkdir(exist_ok=True)
    try:
        for name in ('verified.rmathpp','plot2d.svg','plot2d.png'):
            path=folder/name
            subprocess.run(['debugfs','-R',f'dump /home/test/{name} {path}',str(root)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            assert path.is_file(),name
        import struct, zlib, xml.etree.ElementTree as ET
        raw=(folder/'verified.rmathpp').read_bytes()
        assert raw[:4]==b'RMPP' and struct.unpack_from('<I',raw,8)[0]==len(raw)
        assert struct.unpack_from('<I',raw,12)[0]==zlib.crc32(raw[16:])
        assert struct.unpack_from('<I',raw,24)[0]==5 and b'QemuPlot' in raw and b'x^2/4' in raw
        svg=ET.parse(folder/'plot2d.svg').getroot();assert len(svg)>500
        from PIL import Image
        image=Image.open(folder/'plot2d.png').convert('RGB');assert image.width>500 and len(image.getcolors(1000000))>30
        record=next(item for item in metadata['injected_files'] if item['guest_path'].endswith('/reliefmathpp.elf'))
        executable=folder/'reliefmathpp.elf'
        subprocess.run(['debugfs','-R',f'dump {record["guest_path"]} {executable}',str(root)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        assert fltk.sha256(executable)==record['sha256']==fltk.sha256(OUT/'userland/reliefmathpp.elf')
        (output/'persisted-verification.json').write_text(json.dumps({'native_document_crc':True,'object_count':5,'formula':True,'svg':True,'png':True,'final_executable':True},indent=2)+'\n')
    finally:
        root.unlink(missing_ok=True)


def verify_gui_artifacts(output):
    log=(output/'serial.log').read_text(errors='replace')
    for marker in ('saved objects=4','saved objects=5','calculation mode=3 value=9',
                   'calculation mode=11 value=13','opened objects=5','w=1000 h=680'):
        assert marker in log,'Missing GUI evidence: '+marker
    assert len(re.findall(r'RMP_EXIT status=0',log))==2
    from PIL import Image
    for name in ('reliefmathpp-function-edit','reliefmathpp-3d','reliefmathpp-3d-rotated',
                 'reliefmathpp-calculation','reliefmathpp-surface-calculation',
                 'reliefmathpp-minimum-size','reliefmathpp-reopened'):
        path=output/(name+'.png');assert path.is_file() and path.stat().st_size>10000
        with Image.open(path) as picture:picture.verify()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image',type=Path,default=fltk.DEFAULT_IMAGE)
    parser.add_argument('--kernel',type=Path,default=fltk.DEFAULT_KERNEL)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--interactive',action='store_true')
    parser.add_argument('--verify-existing',action='store_true',help='Resume offline verification of a completed GUI run; requires its logs and all evidence')
    args=parser.parse_args();output=args.output.resolve()
    if args.verify_existing:
        metadata=json.loads((output/'prepare.json').read_text())
        verify_gui_artifacts(output)
        assert fltk.sha256(Path(metadata['source_image']))==metadata['source_image_sha256_before']
        verify_disk(output,metadata)
        (output/'acceptance.json').write_text(json.dumps({'passed':True,'platform':'QEMU KVM / ReliefOS Xorg','source_image_unchanged':True,'resumed_offline_verification':True,'vmware':'not verified'},indent=2)+'\n')
        print('ReliefMath++ GUI evidence and independent persisted checks passed',flush=True)
        return
    metadata=prepare(args.image.resolve(),args.kernel.resolve(),output);s=None
    try:
        s=Session(output);s.login()
        if args.interactive:
            s.launch();code.interact(local={'s':s,'metadata':metadata,'time':time,'filename':filename,'wait_focus':wait_focus,'resize':resize,'dialog_origin':dialog_origin})
        else:
            exercise(s)
    finally:
        if s:s.close()
    assert fltk.sha256(args.image)==metadata['source_image_sha256_before']
    if not args.interactive:
        verify_gui_artifacts(output)
        verify_disk(output,metadata)
        (output/'acceptance.json').write_text(json.dumps({'passed':True,'platform':'QEMU KVM / ReliefOS Xorg','source_image_unchanged':True,'vmware':'not verified'},indent=2)+'\n')
        print('ReliefMath++ QEMU acceptance and independent persisted checks passed',flush=True)

if __name__=='__main__':main()
