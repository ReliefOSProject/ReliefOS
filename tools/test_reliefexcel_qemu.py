#!/usr/bin/env python3
"""Private-image ReliefExcel GUI acceptance helpers (never modify the source disk)."""
from pathlib import Path
import argparse
import json
import os
import re
import subprocess
import time

import test_fltk_apps_qemu as fltk
import test_vdso_qemu as vdso
import test_xorg_vm_qemu as xorg
from test_installer_accounts_qemu import Probe as AccountsProbe
from test_installer_window_qemu import wait_log
from qmp_terminal_smoke import text_keys
from run_gcc_probe_qemu import qmp_quit

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'out/xorg-vmware-debug'


class Probe(AccountsProbe):
    def text(self, text):
        punctuation = {';':'semicolon','(':'shift-9',')':'shift-0','>':'shift-dot',
                       '$':'shift-4','?':'shift-slash',"'":'apostrophe','"':'shift-apostrophe',
                       '!':'shift-1',',':'comma','+':'shift-equal','<':'shift-comma',
                       '*':'shift-8','%':'shift-5','[':'bracket_left',']':'bracket_right'}
        for char in text:
            self.key(punctuation.get(char, text_keys(char)[0]))
        time.sleep(.5)


def prepare(image: Path, kernel: Path, output: Path):
    output.mkdir(parents=True, exist_ok=False)
    task_env={**os.environ,'TMPDIR':str(ROOT/'out/tmp')}
    (ROOT/'out/tmp').mkdir(parents=True,exist_ok=True)
    core=[ROOT/'userland/apps/reliefexcel'/name for name in ('model.cpp','edit.cpp','formula.cpp','document.cpp')]
    fixtures=output/'fixtures'; fixtures.mkdir()
    with (output/'host-helper-build.log').open('w') as log:
        for source,binary in [('reliefexcel_scale_benchmark.cpp','scale-benchmark'),('reliefexcel_guest_verify.cpp','guest-verify')]:
            subprocess.run([os.environ.get('HOSTCXX','c++'),'-std=c++17','-O2','-fno-exceptions','-fno-rtti',
                            '-I'+str(ROOT/'userland/apps/reliefexcel'),str(ROOT/'tools/tests'/source),
                            *map(str,core),'-lm','-o',str(output/binary)],cwd=ROOT,env=task_env,stdout=log,stderr=log,check=True)
    with (output/'host-scale.log').open('w') as log:
        subprocess.run([str(output/'scale-benchmark'),str(fixtures)],env=task_env,stdout=log,stderr=log,check=True)
    probe = fltk.target_compile_probe(output, OUT / 'sysroot/musl', 'clang++', 'ld.lld')
    commands=json.loads((output/'probe-build.command.json').read_text())
    commands['compile'][-3]=str(ROOT/'tools/tests/reliefexcel_clipboard.cpp')
    commands['compile'][-1]=str(output/'clipboard.o')
    commands['link'][commands['link'].index(str(output/'fltk_ui_probe.o'))]=str(output/'clipboard.o')
    commands['link'][commands['link'].index(str(probe))]=str(output/'clipboard.elf')
    with (output/'clipboard-build.log').open('w') as log:
        for command in commands.values(): subprocess.run(command,cwd=ROOT,env=task_env,stdout=log,stderr=log,check=True)
    runner = output / 'rx-runner'
    runner.write_text('''#!/bin/sh
export LC_ALL=zh_CN.UTF-8 LANG=zh_CN.UTF-8
/usr/lib/reliefos/tests/reliefexcel-clipboard >/dev/ttyS0 2>&1 &
clipboard_pid=$!
sleep 1
/usr/lib/reliefos/apps/reliefexcel/reliefexcel.elf "$@" >/dev/ttyS0 2>&1 &
pid=$!
echo "RX_START pid=$pid" >/dev/ttyS0
/usr/lib/reliefos/tests/fltk-ui-probe --watch-window "$pid" >/dev/ttyS0 2>&1 &
wait "$pid"
status=$?
kill "$clipboard_pid"
echo "RX_EXIT status=$status" >/dev/ttyS0
''')
    entry=output/'reliefexcel-entry'
    entry.write_text('#!/bin/sh\nexec /usr/lib/reliefos/apps/reliefexcel/reliefexcel.elf "$@"\n')
    for app in ('reliefexcel','fileman'):
        fltk.validate_target_elf(OUT/f'userland/{app}.elf',sysroot=OUT/'sysroot/musl',required_needed=('libfltk.so.1.3',))
    payload = [
        (OUT/'userland/reliefexcel.elf','/usr/lib/reliefos/apps/reliefexcel/reliefexcel.elf',0o100755),
        (OUT/'userland/fileman.elf','/usr/lib/reliefos/apps/fileman/fileman.elf',0o100755),
        (OUT/'generated/nls/zh_CN/LC_MESSAGES/leonos.mo',fltk.LOCALE_GUEST,0o100644),
        (ROOT/'userland/apps/reliefexcel/reliefexcel.desktop','/usr/share/applications/reliefexcel.desktop',0o100644),
        (ROOT/'resources/build-art/app-icons/reliefexcel.bmp','/usr/lib/reliefos/apps/reliefexcel/reliefexcel.bmp',0o100644),
        (entry,'/usr/bin/reliefexcel',0o100755),
        (runner,'/usr/bin/rx',0o100755), (probe,fltk.PROBE_GUEST,0o100755),
        (output/'clipboard.elf','/usr/lib/reliefos/tests/reliefexcel-clipboard',0o100755),
    ]
    for name in ('scale.csv','scale.rexl'):
        payload.append((fixtures/name,'/home/test/'+name,0o100644))
    metadata=fltk.prepare_disk(image,kernel,output,payload)
    (output/'prepare.json').write_text(json.dumps(metadata,ensure_ascii=False,indent=2)+'\n')
    return metadata


class Session:
    def __init__(self, output: Path):
        self.output=output
        self.serial=output/'serial.log'
        self.qmp=output/'qmp.sock'
        command=['qemu-system-x86_64','-enable-kvm','-cpu','host','-machine','q35',
                 '-m','4096','-smp','2','-bios','/usr/share/edk2/x64/OVMF.4m.fd',
                 '-display','none','-serial',f'file:{self.serial}','-vga','vmware',
                 '-device','qemu-xhci','-device','usb-tablet','-netdev','user,id=net0',
                 '-device','e1000,netdev=net0','-drive',f'file={output}/guest-private.raw,format=raw,if=ide',
                 '-qmp',f'unix:{self.qmp},server=on,wait=off','-no-reboot','-no-shutdown']
        self.log=(output/'qemu.log').open('w')
        self.process=subprocess.Popen(command,cwd=ROOT,stdout=self.log,stderr=self.log,start_new_session=True)
        deadline=time.monotonic()+15
        while not self.qmp.exists():
            if self.process.poll() is not None or time.monotonic()>deadline: raise RuntimeError('QEMU startup failed')
            time.sleep(.1)
        self.probe=Probe(self.qmp,output)

    def login(self):
        wait_log(self.serial,'boot complete:',self.process,120)
        xorg.greeter_login(self.probe,self.process)
        xorg.wait_session(self.probe,self.process,'icewm-ready')

    def launch(self, argument=''):
        offset=len(self.serial.read_text(errors='replace'))
        self.probe.click(300,200)
        self.probe.text('rx'+(' '+argument if argument else ''))
        self.probe.key('ret')
        started=time.monotonic()
        deadline=time.monotonic()+60
        while time.monotonic()<deadline:
            log=self.serial.read_text(errors='replace')[offset:]
            match=re.search(r'RX_START pid=(\d+)',log)
            if match and '[reliefexcel.elf] FLTK spreadsheet ready' in log:
                self.pid=int(match[1]); self.geometry=fltk.wait_client_geometry(self.serial,self.process,self.pid,offset)
                self.ready_seconds=time.monotonic()-started
                self.probe.frame('reliefexcel-start')
                return self.geometry
            if self.process.poll() is not None: raise RuntimeError('QEMU exited')
            time.sleep(.3)
        raise AssertionError('ReliefExcel did not become ready')

    def click(self,x,y):
        fltk.click_client(self.probe,self.geometry,x,y)

    def address(self,address):
        self.click(50,152)
        for key in ('home','shift-end','backspace'): self.probe.key(key)
        self.probe.text(address); self.probe.key('ret')
        time.sleep(.25)

    def enter(self,address,value):
        self.address(address); self.probe.key('f2')
        for key in ('home','shift-end','backspace'): self.probe.key(key)
        self.probe.text(value); self.probe.key('ret')

    def frame(self,name):
        time.sleep(.4)
        return self.probe.frame(name)

    def wait_idle(self,timeout=90):
        deadline=time.monotonic()+timeout
        stable=0
        while time.monotonic()<deadline:
            frame=self.frame('operation-status')
            x,y,w,h=self.geometry
            text=fltk.tesseract_ocr(frame.crop((x+4,y+h-27,x+w//2,y+h-1)),self.output,'operation-status',psm=7,scale=3)
            compact=re.sub(r'\s+','',text)
            busy='%' in compact or '正在' in compact or 'Working' in compact
            stable=stable+1 if text.strip() and not busy else 0
            if stable>=2: return
        raise AssertionError('ReliefExcel operation did not finish')

    def close(self):
        try: self.probe.close()
        finally:
            try: qmp_quit(self.qmp,self.process)
            finally: self.log.close()


def exercise_gui(s):
    s.launch(); s.frame('initial-ribbon')
    s.click(36,17); s.click(40,156); s.frame('ledger')
    s.click(142,s.geometry[3]-47)
    s.frame('summary')
    s.address('D12'); s.probe.key('f2'); s.probe.key('ctrl-v'); s.probe.key('ret')
    s.enter('E12','=SUM(1,2,3)'); s.enter('E13','7'); s.enter('F12','=$E$12+E12')
    s.address('F12'); s.probe.key('ctrl-c'); s.address('F13'); s.probe.key('ctrl-v')
    s.frame('summary-and-chinese')
    s.address('E12'); s.click(250,152); s.probe.key('home'); s.probe.key('shift-end')
    s.probe.text('=999'); s.probe.key('esc'); s.address('E13')
    s.address('E20')
    for number in range(1,10):
        for key in (str(number),'ret'):
            s.probe.command('human-monitor-command',{'command-line':f'sendkey {key} 20'})
            time.sleep(.04)
    s.wait_idle(); s.frame('continuous-input')
    s.click(205,s.geometry[3]-47)
    for address,value in [('A1','key'),('B1','label'),('C1','sentinel'),('A2','2'),
                          ('B2','b'),('C2','20'),('A3','1'),('B3','a'),('C3','10')]: s.enter(address,value)
    s.address('A1')
    for key in ('shift-down','shift-down','shift-right'): s.probe.key(key)
    s.click(344,17); s.click(70,55)
    center_x=s.geometry[0]+s.geometry[2]//2; center_y=s.geometry[1]+s.geometry[3]//2
    s.probe.click(center_x-290+505,center_y-100+167)
    s.frame('bounded-sort')
    s.click(210,55); s.probe.click(center_x+10,center_y-130+148); s.probe.text('1')
    s.probe.click(center_x-290+505,center_y-130+227); s.frame('filtered')
    s.click(210,84); s.click(420,17); s.click(70,84); s.address('A23'); s.frame('frozen-panes')
    s.probe.key('ctrl-s'); time.sleep(.5)
    # FLTK's chooser completes absolute paths while typing. Clear its entire
    # input and use a basename in the initial /home/test directory.
    s.probe.click(240,345)
    for key in ('home','shift-end','backspace'): s.probe.key(key)
    s.probe.text('acceptance.rexl'); s.probe.key('ret'); s.frame('workbook-saved')
    s.probe.key('alt-f4'); time.sleep(.5)
    s.launch('/home/test/acceptance.rexl'); s.frame('workbook-reopened')
    s.probe.key('alt-f4'); time.sleep(.5)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image',required=True,type=Path)
    parser.add_argument('--kernel',required=True,type=Path)
    parser.add_argument('--output',required=True,type=Path,help='new evidence directory')
    parser.add_argument('--exercise',action='store_true',help='also exercise Chinese editing, sort/filter, freeze and native save/reopen')
    args=parser.parse_args()
    output=args.output.resolve()
    metadata=prepare(args.image.resolve(),args.kernel.resolve(),output)
    session=None
    try:
        session=Session(output); session.login()
        if args.exercise: exercise_gui(session)
        session.launch('/home/test/scale.rexl')
        opened_seconds=session.ready_seconds
        session.frame('scale-opened')
        session.enter('A1','-1'); session.wait_idle()
        session.probe.key('ctrl-s'); session.wait_idle(); session.frame('scale-saved')
        session.click(142,session.geometry[3]-47); session.frame('scale-summary')
        session.probe.key('alt-f4'); time.sleep(.5)
    finally:
        if session is not None: session.close()
    extracted=output/'extracted'; extracted.mkdir()
    part=next(p for p in metadata['gpt_partitions'] if p['name']=='RELIEFOS_ROOT')
    root=vdso.extract_root(output/'guest-private.raw',{'root_first':int(part['first']),'root_last':int(part['last'])},extracted)
    verified=[]
    for name,mode in [('scale.rexl','scale-edited')]+([('acceptance.rexl','gui-burst')] if args.exercise else []):
        destination=extracted/name
        subprocess.run(['debugfs','-R',f'dump /home/test/{name} {fltk.debugfs_quote(destination)}',str(root)],check=True)
        subprocess.run([str(output/'guest-verify'),str(destination),mode],check=True)
        verified.append({'file':name,'sha256':fltk.sha256(destination),'mode':mode})
    unchanged=fltk.sha256(args.image)==metadata['source_image_sha256_before']
    if not unchanged: raise RuntimeError('source image was modified')
    result={'passed':True,'source_image_unchanged':True,'scale_ready_seconds':opened_seconds,
            'verified_readbacks':verified,'vmware_verified':False}
    (output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))
    return 0


if __name__=='__main__':
    raise SystemExit(main())
