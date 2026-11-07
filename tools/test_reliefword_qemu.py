#!/usr/bin/env python3
"""Exercise ReliefWord in a private QEMU image, then independently inspect saved files."""
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
from test_reliefexcel_qemu import Session as BaseSession
from test_installer_window_qemu import wait_log

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'out/xorg-vmware-debug'


def prepare(image,kernel,output):
    output.mkdir(parents=True,exist_ok=False)
    task_env={**os.environ,'TMPDIR':str(ROOT/'out/tmp')}
    (ROOT/'out/tmp').mkdir(parents=True,exist_ok=True)
    fixtures=output/'fixtures'; fixtures.mkdir()
    core=[ROOT/'userland/apps/reliefword'/name for name in ('model.cpp','document.cpp')]
    with (output/'host-helper-build.log').open('w') as log:
        subprocess.run([os.environ.get('HOSTCXX','c++'),'-std=c++17','-O2','-Wall','-Wextra','-Werror','-fno-exceptions','-fno-rtti',
                        '-I'+str(ROOT/'userland/apps/reliefword'),str(ROOT/'tools/tests/reliefword_guest_verify.cpp'),
                        *map(str,core),'-lm','-o',str(output/'guest-verify')],cwd=ROOT,env=task_env,stdout=log,stderr=log,check=True)
    subprocess.run([str(output/'guest-verify'),'prepare',str(fixtures)],cwd=ROOT,env=task_env,check=True)
    probe=fltk.target_compile_probe(output,OUT/'sysroot/musl','clang++','ld.lld')
    template=json.loads((output/'probe-build.command.json').read_text())
    for name,source in [('clipboard','reliefexcel_clipboard.cpp'),('icon-probe','x11_app_icon_probe.cpp')]:
        commands={key:list(value) for key,value in template.items()}
        commands['compile'][-3]=str(ROOT/'tools/tests'/source)
        commands['compile'][-1]=str(output/(name+'.o'))
        commands['link'][commands['link'].index(str(output/'fltk_ui_probe.o'))]=str(output/(name+'.o'))
        commands['link'][commands['link'].index(str(probe))]=str(output/(name+'.elf'))
        with (output/(name+'-build.log')).open('w') as log:
            for command in commands.values(): subprocess.run(command,cwd=ROOT,env=task_env,stdout=log,stderr=log,check=True)
    runner=output/'rw-runner'
    runner.write_text('''#!/bin/sh
export LC_ALL=zh_CN.UTF-8 LANG=zh_CN.UTF-8
/usr/lib/reliefos/tests/reliefword-clipboard >/dev/ttyS0 2>&1 &
clipboard_pid=$!
sleep 1
/usr/lib/reliefos/apps/reliefword/reliefword.elf "$@" >/dev/ttyS0 2>&1 &
pid=$!
echo "RW_START pid=$pid" >/dev/ttyS0
crash_pid=
if [ -n "$RW_CRASH_AFTER" ]; then
    (sleep "$RW_CRASH_AFTER"; kill -KILL "$pid") &
    crash_pid=$!
fi
/usr/lib/reliefos/tests/fltk-ui-probe --watch-window-focus "$pid" >/dev/ttyS0 2>&1 &
probe_pid=$!
/usr/lib/reliefos/tests/reliefword-icon-probe "$pid" reliefword >/dev/ttyS0 2>&1
echo "RW_ICON_STATUS status=$?" >/dev/ttyS0
wait "$pid"
status=$?
kill "$clipboard_pid" "$probe_pid"
if [ -n "$crash_pid" ]; then kill "$crash_pid" 2>/dev/null; fi
echo "RW_EXIT status=$status" >/dev/ttyS0
''')
    entry=output/'reliefword-entry'
    entry.write_text('#!/bin/sh\nexec /usr/lib/reliefos/apps/reliefword/reliefword.elf "$@"\n')
    for app in ('reliefword','fileman'):
        fltk.validate_target_elf(OUT/f'userland/{app}.elf',sysroot=OUT/'sysroot/musl',required_needed=('libfltk.so.1.3',))
    payload=[
        (OUT/'userland/reliefword.elf','/usr/lib/reliefos/apps/reliefword/reliefword.elf',0o100755),
        (OUT/'userland/fileman.elf','/usr/lib/reliefos/apps/fileman/fileman.elf',0o100755),
        (OUT/'generated/nls/zh_CN/LC_MESSAGES/leonos.mo',fltk.LOCALE_GUEST,0o100644),
        (ROOT/'userland/apps/reliefword/reliefword.desktop','/usr/share/applications/reliefword.desktop',0o100644),
        (ROOT/'resources/build-art/app-icons/reliefword.bmp','/usr/lib/reliefos/apps/reliefword/reliefword.bmp',0o100644),
        (entry,'/usr/bin/reliefword',0o100755),(runner,'/usr/bin/rw',0o100755),
        (probe,fltk.PROBE_GUEST,0o100755),
        (output/'clipboard.elf','/usr/lib/reliefos/tests/reliefword-clipboard',0o100755),
        (output/'icon-probe.elf','/usr/lib/reliefos/tests/reliefword-icon-probe',0o100755),
    ]
    for size in (16,32,48,64,128,256):
        path=ROOT/f'resources/build-art/app-icons/hicolor/{size}x{size}/apps/reliefos-reliefword.png'
        payload.append((path,f'/usr/share/icons/hicolor/{size}x{size}/apps/{path.name}',0o100644))
    for path in sorted(fixtures.iterdir()): payload.append((path,'/home/test/'+path.name,0o100644))
    metadata=fltk.prepare_disk(image,kernel,output,payload)
    (output/'prepare.json').write_text(json.dumps(metadata,ensure_ascii=False,indent=2)+'\n')
    return metadata


class Session(BaseSession):
    def __init__(self,output):
        super().__init__(output)
        original=self.probe.command
        def command(name,arguments=None):
            result=original(name,arguments)
            if name=='human-monitor-command' and arguments and arguments.get('command-line','').startswith('sendkey '):
                with (self.output/'keyboard.jsonl').open('a') as log:
                    log.write(json.dumps({'time':time.monotonic(),'command':arguments['command-line'],'response':result})+'\n')
                if isinstance(result,str) and result.strip(): raise AssertionError('QMP rejected keyboard input: '+result)
            return result
        self.probe.command=command

    def launch(self,argument='',*,crash_after=None,recover=False,import_warning=False):
        offset=len(self.serial.read_text(errors='replace'))
        self.launch_offset=offset
        self.probe.click(300,200)
        prefix=f'RW_CRASH_AFTER={crash_after} ' if crash_after else ''
        self.probe.text(prefix+'rw'+(' '+argument if argument else '')); self.probe.key('ret')
        deadline=time.monotonic()+60
        prompted=False
        while time.monotonic()<deadline:
            log=self.serial.read_text(errors='replace')[offset:]
            match=re.search(r'RW_START pid=(\d+)',log)
            if match and not prompted and (recover or import_warning):
                time.sleep(2)
                self.probe.frame('reliefword-startup-prompt')
                # FLTK fl_choice focuses its middle Return button. Import is
                # that button; Recover is the preceding button in tab order.
                if recover: self.keys('shift-tab','spc')
                else: self.keys('ret')
                prompted=True
            if match and '[reliefword.elf] FLTK word processor ready' in log:
                self.pid=int(match[1]); self.geometry=fltk.wait_client_geometry(self.serial,self.process,self.pid,offset)
                xorg.wait_serial_after(self.serial,'APP_ICON_PASS app=reliefword ',self.process,offset,timeout=40)
                wait_editor_focus(self)
                return self.geometry
            if self.process.poll() is not None: raise RuntimeError('QEMU exited')
            time.sleep(.3)
        raise AssertionError('ReliefWord did not become ready')

    def frame(self,name):
        frame=super().frame(name)
        if 'RW_EXIT status=' in self.serial.read_text(errors='replace')[getattr(self,'launch_offset',0):]:
            raise AssertionError('ReliefWord exited before screenshot '+name)
        return frame

    def keys(self,*keys):
        for key in keys: self.probe.key(key)

    def fill(self,value):
        self.keys('home','shift-end','backspace'); self.probe.text(value)


def export_document(session,format,name,expect_alert=False):
    session.click(36,17)
    session.probe.frame('export-'+format+'-menu')
    # Menu rows use the app's fixed 14px FLTK font (19px item pitch).
    # Readback below verifies that the intended export, rather than another
    # menu command, actually produced the expected content and formatting.
    session.click(70,41+19*(7 if format=='HTML' else 6))
    time.sleep(.25)
    if format=='TXT': session.probe.frame('txt-format-warning'); session.keys('ret')
    choose_filename(session,name,'export-'+format+'-chooser',wait_editor=not expect_alert)
    session.frame('reliefword-exported-'+format)


def choose_filename(session,name,tag,wait_editor=True):
    wait_editor_focus(session,focused=False)
    time.sleep(.4); session.probe.frame(tag)
    # Fl_File_Chooser::show() focuses its filename input. Its hotspot follows
    # the mouse, so screen coordinates differ between separate app launches.
    session.fill(name)
    if wait_editor: editor_focus_after(session,'ret')
    else: session.keys('ret'); time.sleep(.5)


def wait_editor_focus(session,focused=True,after=None):
    offset=session.launch_offset if after is None else after
    pattern=rf'FLTK_APP_FOCUS pid={session.pid} focused=([01])'
    deadline=time.monotonic()+25
    stable_since=None
    while time.monotonic()<deadline:
        log=session.serial.read_text(errors='replace')[offset:]
        states=re.findall(pattern,log)
        if states and (states[-1]=='1')==focused:
            if stable_since is None: stable_since=time.monotonic()
            if time.monotonic()-stable_since>=.4: return
        else: stable_since=None
        if session.process.poll() is not None: raise RuntimeError('QEMU exited while waiting for focus')
        if 'RW_EXIT status=' in log: raise AssertionError('ReliefWord exited while waiting for focus')
        time.sleep(.05)
    raise AssertionError(f'ReliefWord focus did not settle: pid={session.pid} focused={focused}')


def editor_focus_after(session,*keys):
    offset=len(session.serial.read_text(errors='replace'))
    session.keys(*keys)
    wait_editor_focus(session,after=offset)


def close_document(session):
    offset=len(session.serial.read_text(errors='replace'))
    session.keys('alt-f4')
    xorg.wait_serial_after(session.serial,'RW_EXIT status=0',session.process,offset,timeout=20)
    time.sleep(.4)


def exercise(s):
    s.launch('/home/test/word-fixture.rword'); frame=s.frame('reliefword-document')
    visible=fltk.tesseract_ocr(frame,s.output,'word-chinese',psm=11)
    if fltk.cjk_count(visible)<40: raise AssertionError('Chinese document/UI text was not visible')
    print('Word ready; initial document captured',flush=True)
    original=s.geometry
    for width,height,tag in [(860,540,'minimum'),(original[2],original[3],'restored')]:
        x,y,w,h=s.geometry; offset=len(s.serial.read_text(errors='replace'))
        s.probe.move(x+w+2,y+h+2); s.probe.button(True); time.sleep(.15)
        s.probe.move(x+width+2,y+height+2); time.sleep(.25); s.probe.button(False)
        s.geometry=fltk.wait_client_geometry(s.serial,s.process,s.pid,offset,width,height)
        frame=s.frame('reliefword-'+tag)
        x,y,w,h=s.geometry
        status=fltk.tesseract_ocr(frame.crop((x+w-90,y+h-28,x+w-10,y+h-2)),s.output,'word-status-'+tag,psm=7,language='eng',scale=4)
        if not re.search(r'100\s*%',status): raise AssertionError('Status bar was clipped after resize: '+status)
    s.keys('ctrl-end','ret','ctrl-v','ret'); s.probe.text('WordBench: ')
    for digit in '123456789':
        s.probe.command('human-monitor-command',{'command-line':f'sendkey {digit} 20'})
        time.sleep(.04)
    time.sleep(.4)
    s.frame('edit-01-typed')
    s.keys('home','shift-end','ctrl-b','ctrl-e')
    s.click(340,55); s.keys('ctrl-a'); s.probe.text('18'); s.keys('ret')
    s.frame('edit-02-formatted')
    s.keys('ctrl-c','ctrl-end','ret','ctrl-v','ctrl-z','ctrl-y')
    s.frame('edit-03-copied')
    s.keys('ctrl-h'); s.probe.text('WordBench'); s.keys('tab'); s.probe.text('ReliefWordCheck')
    s.frame('edit-04-find')
    x,y,w,h=s.geometry
    s.probe.click(x+w-460+355,y+115+168); s.frame('edit-05-replaced')
    editor_focus_after(s,'esc'); s.frame('edit-06-search-closed')
    s.keys('ctrl-end'); s.frame('edit-07-document-end')
    s.keys('ctrl-ret'); s.frame('edit-08-page-break')
    s.probe.text('Second page confirmation'); s.frame('edit-09-second-page')
    s.click(210,17); s.click(60,60); time.sleep(.3)
    # Page setup is centered on the desktop work area (same placement as FLTK).
    s.frame('reliefword-page-dialog')
    # Derive the screen size instead of assuming a display resolution.
    frame=s.probe.frame('page-setup-position'); sw,sh=frame.size
    dx=(sw-480)//2; dy=(sh-30-500)//2
    s.probe.click(dx+340,dy+54); s.keys('home','down','ret')
    offset=len(s.serial.read_text(errors='replace'))
    s.probe.click(dx+410,dy+468); wait_editor_focus(s,after=offset)
    s.keys('ctrl-s'); time.sleep(.6); s.frame('reliefword-edited')
    export_document(s,'HTML','acceptance.html')
    export_document(s,'TXT','acceptance.txt')
    close_document(s)
    s.launch('/home/test/word-fixture.rword'); s.keys('ctrl-end'); s.frame('reliefword-reopened')
    close_document(s)
    return [('word-fixture.rword','native'),('acceptance.html','html'),('acceptance.txt','txt')]


def exercise_imports(s):
    s.launch('/home/test/import-source.html',import_warning=True); s.frame('reliefword-html-import')
    export_document(s,'HTML','import-source.html',expect_alert=True)
    s.frame('reliefword-source-protected')
    # Whole-screen OCR is unreliable for a small FLTK alert. The saved HTML
    # source SHA is checked below after the VM exits; overwriting it with the
    # exporter would change its bytes, so this is a direct protection check.
    editor_focus_after(s,'ret'); s.keys('ctrl-s'); choose_filename(s,'imported.rword','html-native-chooser')
    close_document(s)
    s.launch('/home/test/import-source.txt'); s.frame('reliefword-txt-import')
    s.keys('ctrl-s'); choose_filename(s,'text-imported.rword','text-native-chooser')
    close_document(s)
    return [('imported.rword','html-import'),('text-imported.rword','txt-import')]


def exercise_recovery(s):
    s.launch('/home/test/word-fixture.rword',crash_after=75)
    s.keys('ctrl-end','ret'); s.probe.text('Recovery marker 2468')
    print('Recovery test: waiting for the real 60-second autosave timer',flush=True)
    time.sleep(62); s.frame('reliefword-recovery-copy')
    xorg.wait_serial_after(s.serial,'RW_EXIT status=137',s.process,s.launch_offset,timeout=35)
    time.sleep(.5)
    s.launch(recover=True); s.keys('ctrl-end'); s.frame('reliefword-recovered')
    s.keys('ctrl-s'); choose_filename(s,'recovered.rword','recovery-native-chooser')
    close_document(s)
    return [('recovered.rword','recovery')]


def exercise_branch(s):
    s.launch('/home/test/word-fixture.rword'); s.keys('ctrl-n'); s.probe.text('A')
    s.keys('ctrl-s'); choose_filename(s,'branch.rword','branch-native-chooser')
    s.keys('ctrl-z'); s.probe.text('B'); s.keys('alt-f4')
    s.frame('reliefword-branch-close-prompt')
    editor_focus_after(s,'esc'); s.frame('reliefword-branch-still-open')
    s.keys('ctrl-s'); time.sleep(.4); close_document(s)
    return [('branch.rword','branch')]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image',required=True,type=Path)
    parser.add_argument('--kernel',required=True,type=Path)
    parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--recovery',action='store_true',help='also wait for the 60-second autosave and recover after SIGKILL')
    args=parser.parse_args(); output=args.output.resolve()
    metadata=prepare(args.image.resolve(),args.kernel.resolve(),output)
    session=None
    try:
        session=Session(output)
        wait_log(session.serial,'boot complete:',session.process,120)
        xorg.wait_greeter_stage(session.probe,session.process,'login','word-greeter',timeout=120)
        session.login(); expected=exercise(session)+exercise_imports(session)+exercise_branch(session)
        if args.recovery: expected+=exercise_recovery(session)
    finally:
        if session is not None: session.close()
    extracted=output/'extracted'; extracted.mkdir()
    part=next(p for p in metadata['gpt_partitions'] if p['name']=='RELIEFOS_ROOT')
    root=vdso.extract_root(output/'guest-private.raw',{'root_first':int(part['first']),'root_last':int(part['last'])},extracted)
    for name,mode in expected:
        destination=extracted/name
        subprocess.run(['debugfs','-R',f'dump /home/test/{name} {fltk.debugfs_quote(destination)}',str(root)],check=True)
        subprocess.run([str(output/'guest-verify'),str(destination),mode],check=True)
    original=extracted/'import-source.html'
    subprocess.run(['debugfs','-R',f'dump /home/test/import-source.html {fltk.debugfs_quote(original)}',str(root)],check=True)
    if fltk.sha256(original)!=fltk.sha256(output/'fixtures/import-source.html'): raise AssertionError('HTML import source was overwritten')
    if fltk.sha256(args.image)!=metadata['source_image_sha256_before']: raise AssertionError('Source image changed')
    payload=next(row for row in metadata['injected_files'] if row['guest_path']=='/usr/lib/reliefos/apps/reliefword/reliefword.elf')
    if fltk.sha256(OUT/'userland/reliefword.elf')!=payload['sha256']: raise AssertionError('Target ELF changed during validation')
    result={'passed':True,'verified':expected,'source_image_unchanged':True,'html_source_unchanged':True,'vmware_verified':False,
            'target_elf_sha256':payload['sha256']}
    (output/'result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
    print(json.dumps(result,ensure_ascii=False,indent=2),flush=True)


if __name__=='__main__': main()
