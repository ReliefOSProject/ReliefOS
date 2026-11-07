#!/usr/bin/env python3
"""Exercise the real ReliefMath ELF in a hash-checked private QEMU disk."""
from pathlib import Path
import argparse
import code
import json
import os
import re
import subprocess
import time
import socket
import hashlib
import test_fltk_apps_qemu as fltk
import test_vdso_qemu as vdso
import test_xorg_vm_qemu as xorg
from test_reliefexcel_qemu import Session as BaseSession
from test_installer_window_qemu import wait_log

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'out/xorg-vmware-debug'

def prepare(image,kernel,output):
    output.mkdir(parents=True,exist_ok=False)
    env={**os.environ,'TMPDIR':str(ROOT/'out/tmp')}
    fixtures=output/'fixtures';fixtures.mkdir()
    core=ROOT/'userland/apps/reliefmath'
    with (output/'verifier-build.log').open('w') as log:
        subprocess.run(['c++','-std=c++17','-O2','-Wall','-Wextra','-Werror','-nostdinc++','-fno-exceptions','-fno-rtti','-iquote',str(core),str(ROOT/'tools/tests/reliefmath_guest_verify.cpp'),str(core/'model.cpp'),str(core/'document.cpp'),'-o',str(output/'guest-verify')],cwd=ROOT,env=env,stdout=log,stderr=log,check=True)
    subprocess.run([str(output/'guest-verify'),'prepare',str(fixtures)],env=env,check=True)
    probe=fltk.target_compile_probe(output,OUT/'sysroot/musl','clang++','ld.lld')
    template=json.loads((output/'probe-build.command.json').read_text())
    for name,source in [('clipboard','reliefmath_clipboard.cpp'),('icon-probe','x11_app_icon_probe.cpp')]:
        commands={key:list(value) for key,value in template.items()}
        commands['compile'][-3]=str(ROOT/'tools/tests'/source);commands['compile'][-1]=str(output/(name+'.o'))
        commands['link'][commands['link'].index(str(output/'fltk_ui_probe.o'))]=str(output/(name+'.o'))
        commands['link'][commands['link'].index(str(probe))]=str(output/(name+'.elf'))
        with (output/(name+'-build.log')).open('w') as log:
            for command in commands.values():subprocess.run(command,cwd=ROOT,env=env,stdout=log,stderr=log,check=True)
    runner=output/'rm-runner'
    runner.write_text('''#!/bin/sh
export LC_ALL=zh_CN.UTF-8 LANG=zh_CN.UTF-8
/usr/lib/reliefos/tests/reliefmath-clipboard >/dev/ttyS0 2>&1 &
clipboard_pid=$!
/usr/lib/reliefos/apps/reliefmath/reliefmath.elf "$@" >/dev/ttyS0 2>&1 &
pid=$!
echo "RM_START pid=$pid" >/dev/ttyS0
crash_pid=
if [ -n "$RM_CRASH_AFTER" ]; then
 (sleep "$RM_CRASH_AFTER"; kill -KILL "$pid") &
 crash_pid=$!
fi
if [ -n "$RM_CONFLICT_AFTER" ]; then
 (sleep "$RM_CONFLICT_AFTER"; cp /home/test/partial.rmath /home/test/conflict.rmath) &
fi
if [ -n "$RM_WORKER_FAIL_AFTER" ]; then
 (sleep "$RM_WORKER_FAIL_AFTER"; for child in $(pgrep -P "$pid"); do
   echo "RM_WORKER_KILL parent=$pid child=$child" >/dev/ttyS0
   kill -KILL "$child"
  done) &
fi
/usr/lib/reliefos/tests/fltk-ui-probe --watch-window-focus "$pid" >/dev/ttyS0 2>&1 &
probe_pid=$!
/usr/lib/reliefos/tests/reliefmath-icon-probe "$pid" reliefmath >/dev/ttyS0 2>&1
wait "$pid"
status=$?
kill "$clipboard_pid" "$probe_pid" 2>/dev/null
if [ -n "$crash_pid" ]; then kill "$crash_pid" 2>/dev/null; fi
echo "RM_EXIT status=$status" >/dev/ttyS0
''')
    entry=output/'reliefmath-entry';entry.write_text('#!/bin/sh\nexec /usr/lib/reliefos/apps/reliefmath/reliefmath.elf "$@"\n')
    payload=[(OUT/'userland/reliefmath.elf','/usr/lib/reliefos/apps/reliefmath/reliefmath.elf',0o100755),
             (OUT/'userland/reliefmath-render.elf','/usr/lib/reliefos/apps/reliefmath/reliefmath-render.elf',0o100755),
             (OUT/'tests/reliefmath-backend-probe.elf','/usr/lib/reliefos/tests/reliefmath-backend-probe',0o100755),
             (OUT/'userland/fileman.elf','/usr/lib/reliefos/apps/fileman/fileman.elf',0o100755),
             (OUT/'generated/nls/zh_CN/LC_MESSAGES/leonos.mo',fltk.LOCALE_GUEST,0o100644),
             (ROOT/'resources/reliefmath/STIXTwoMath-Regular.otf','/usr/share/reliefmath/STIXTwoMath-Regular.otf',0o100444),
             (ROOT/'userland/apps/reliefmath/reliefmath.desktop','/usr/share/applications/reliefos-reliefmath.desktop',0o100644),
             (ROOT/'resources/build-art/app-icons/reliefmath.bmp','/usr/lib/reliefos/apps/reliefmath/reliefmath.bmp',0o100644),
             (entry,'/usr/bin/reliefmath',0o100755),(runner,'/usr/bin/rmtest',0o100755),
             (probe,fltk.PROBE_GUEST,0o100755),(output/'clipboard.elf','/usr/lib/reliefos/tests/reliefmath-clipboard',0o100755),
             (output/'icon-probe.elf','/usr/lib/reliefos/tests/reliefmath-icon-probe',0o100755)]
    for lib,soname in [('libcairo.so','libcairo.so.2'),('libfreetype.so','libfreetype.so.6')]:
        payload.append(((OUT/'upstream/reliefmath-development/usr/lib'/lib).resolve(),'/usr/lib/'+soname,0o100755))
    for size in (16,32,48,64,128,256):
        path=ROOT/f'resources/build-art/app-icons/hicolor/{size}x{size}/apps/reliefos-reliefmath.png';payload.append((path,f'/usr/share/icons/hicolor/{size}x{size}/apps/{path.name}',0o100644))
    for p in fixtures.iterdir():payload.append((p,'/home/test/'+p.name,0o100644))
    for elf,needed in [('reliefmath.elf',('libfltk.so.1.3',)),('reliefmath-render.elf',('libcairo.so.2','libfreetype.so.6'))]:fltk.validate_target_elf(OUT/'userland'/elf,sysroot=OUT/'sysroot/musl',required_needed=needed)
    original_extract=vdso.extract_root
    def extract_sparse(raw,layout,folder):
        root=folder/'guest-root.ext4'
        subprocess.run(['dd','if='+str(raw),'of='+str(root),'bs=512',
                        'skip='+str(layout['root_first']),
                        'count='+str(int(layout['root_last'])-int(layout['root_first'])+1),
                        'conv=sparse','status=none'],check=True)
        return root
    vdso.extract_root=extract_sparse
    try:metadata=fltk.prepare_disk(image,kernel,output,payload)
    finally:vdso.extract_root=original_extract
    Path(metadata['root_image_snapshot']).unlink();metadata['root_image_snapshot_removed']=True
    (output/'prepare.json').write_text(json.dumps(metadata,ensure_ascii=False,indent=2)+'\n')
    return metadata

class Session(BaseSession):
    def __init__(self,output):
        super().__init__(output)
        from qmp_terminal_smoke import text_keys
        def text(value):
            punctuation = {';':'semicolon','(':'shift-9',')':'shift-0','>':'shift-dot',
                           '$':'shift-4','?':'shift-slash',"'":'apostrophe','"':'shift-apostrophe',
                           '!':'shift-1',',':'comma','+':'shift-equal','<':'shift-comma',
                           '*':'shift-8','%':'shift-5','[':'bracket_left',']':'bracket_right',
                           '{':'shift-bracket_left','}':'shift-bracket_right'}
            for ch in value:
                key=punctuation.get(ch,text_keys(ch)[0])
                self.probe.command('human-monitor-command',{'command-line':f'sendkey {key} 20'})
                time.sleep(.055)
            time.sleep(.5)
        self.probe.text=text
        original=self.probe.command
        def command(name,arguments=None):
            result=original(name,arguments)
            if name=='human-monitor-command' and arguments and arguments.get('command-line','').startswith('sendkey '):
                with (self.output/'keyboard.jsonl').open('a') as f:f.write(json.dumps({'time':time.monotonic(),'command':arguments['command-line'],'response':result})+'\n')
                if isinstance(result,str) and result.strip():raise AssertionError('QMP rejected key: '+result)
            return result
        self.probe.command=command
    def login(self):
        wait_log(self.serial,'boot complete:',self.process,120)
        # This debug kernel and disk log every Xorg startup stage; require the
        # actual greeter before using the shared login helper's short timeout.
        xorg.wait_greeter_stage(self.probe,self.process,'login','tty1-greeter',timeout=180)
        xorg.greeter_login(self.probe,self.process)
        xorg.wait_session(self.probe,self.process,'icewm-ready',timeout=90)
    def launch(self,path='/home/test/acceptance.rmath',crash_after=None,conflict_after=None,worker_fail_after=None):
        offset=len(self.serial.read_text(errors='replace'));self.launch_offset=offset
        prefix=''
        if crash_after:prefix+=f'RM_CRASH_AFTER={int(crash_after)} '
        if conflict_after:prefix+=f'RM_CONFLICT_AFTER={int(conflict_after)} '
        if worker_fail_after:prefix+=f'RM_WORKER_FAIL_AFTER={int(worker_fail_after)} '
        self.probe.click(300,200);self.probe.text(prefix+'rmtest'+(' '+path if path else ''));self.probe.key('ret')
        deadline=time.monotonic()+60
        while time.monotonic()<deadline:
            log=self.serial.read_text(errors='replace')[offset:];match=re.search(r'RM_START pid=(\d+)',log)
            if match and '[reliefmath.elf] FLTK formula editor ready' in log:
                self.pid=int(match[1]);self.geometry=fltk.wait_client_geometry(self.serial,self.process,self.pid,offset)
                frame=self.probe.frame('reliefmath-startup-ready')
                frame.save(self.output/'reliefmath-startup-ready.png')
                text=fltk.tesseract_ocr(frame,self.output,'reliefmath-startup-ready')
                if 'LaTeX' not in text:
                    raise AssertionError('ReliefMath reported ready before its client area was drawn')
                time.sleep(3);return self.geometry
            if self.process.poll() is not None:raise RuntimeError('QEMU exited')
            time.sleep(.2)
        raise AssertionError('ReliefMath did not launch')
    def keys(self,*keys):
        for key in keys:self.probe.key(key)
    def fill(self,text):
        # Standard Fl_Input uses Ctrl+A as beginning-of-line on this platform.
        self.keys('home','shift-end','backspace');self.probe.text(text)
    def editor_source(self,source):
        self.click(360,240);self.keys('ctrl-a','backspace');self.probe.text(source);time.sleep(.6)
    def screenshot(self,name):
        self.frame(name);return self.output/(name+'.png')
    def wait_text(self,pattern,tag,timeout=60):
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            frame=self.probe.frame(tag);text=fltk.tesseract_ocr(frame,self.output,tag,psm=11)
            if re.search(pattern,re.sub(r'\s+','',text),re.I):return text
            time.sleep(.4)
        raise AssertionError('Text not observed: '+pattern)

def wait_focus(s,focused=True,timeout=30):
    deadline=time.monotonic()+timeout
    pattern=rf'FLTK_APP_FOCUS pid={s.pid} focused=([01])'
    stable=None
    while time.monotonic()<deadline:
        states=re.findall(pattern,s.serial.read_text(errors='replace')[s.launch_offset:])
        if states and (states[-1]=='1')==focused:
            if stable is None:stable=time.monotonic()
            if time.monotonic()-stable>.4:return
        else:stable=None
        if s.process.poll() is not None:raise AssertionError('QEMU exited')
        time.sleep(.1)
    raise AssertionError(f'Focus did not settle: focused={focused}')

def filename(s,path,tag):
    wait_focus(s,False);frame=s.frame(tag)
    x,y=dialog_origin(frame,500,386)
    s.probe.click(x+300,y+322)
    # This fixture lives in HOME. Fl_File_Input selects its basename while
    # maintaining the directory prefix as slash characters are typed.
    assert Path(path).parent==Path('/home/test')
    s.fill(Path(path).name);s.keys('ret');wait_focus(s,True)

def close_app(s,dirty=False):
    offset=len(s.serial.read_text(errors='replace'));x,y,w,h=s.geometry
    s.probe.click(x+w-10,y-11)
    if dirty:
        s.screenshot('discard-prompt');s.keys('alt-d')
    xorg.wait_serial_after(s.serial,'RM_EXIT status=0',s.process,offset,timeout=60)

def close_export(s):
    x,y=dialog_origin(s.probe.frame('export-complete'),780,428)
    s.probe.click(x+699,y+384);wait_focus(s,True)

def source_clipboard(s):
    offset=len(s.serial.read_text(errors='replace'));s.click(365,210);s.keys('ctrl-a','ctrl-c');time.sleep(1)
    deadline=time.monotonic()+20
    while time.monotonic()<deadline:
        match=re.findall(r'RM_CLIPBOARD_TEXT_HEX ([0-9a-f]*)',s.serial.read_text(errors='replace')[offset:])
        if match:return bytes.fromhex(match[-1]).decode()
        time.sleep(.1)
    raise AssertionError('Clipboard consumer did not read formula text')

def menu(s,row):
    s.click(40,17);s.click(80,41+19*row)

def resize(s,width,height):
    x,y,w,h=s.geometry;offset=len(s.serial.read_text(errors='replace'))
    s.probe.move(x+w+2,y+h+2);s.probe.button(True);time.sleep(.15)
    s.probe.move(x+width+2,y+height+2);time.sleep(.25);s.probe.button(False)
    s.geometry=fltk.wait_client_geometry(s.serial,s.process,s.pid,offset,width,height)

def select_formula(s,row):
    s.click(70,190+(row-1)*15);time.sleep(.5)

def start_export(s,fmt,all_formulas=True,pattern='{index}',directory='/home/test',scale=1):
    s.click(117,17);s.click(928,83 if all_formulas else 55)
    wait_focus(s,False);time.sleep(.3)
    # Native export modal is 690x434 and shown at pointer hotspot. Its visible
    # title rectangle is measured from the framebuffer, not assumed centered.
    frame=s.probe.frame('export-options');origin=dialog_origin(frame,690,434)
    x,y=origin
    def click(a,b):s.probe.click(x+a,y+b)
    click(240,72);s.keys('home');s.keys(*(['down']*fmt));s.keys('ret')
    click(260,112);s.fill(directory)
    click(260,152);s.fill(pattern)
    if scale!=1:click(190,226);s.fill(str(scale))
    click(606,394);wait_focus(s,False)
    return origin

def dialog_origin(frame,width,height):
    # FLTK modal client is a standard grey rectangle. IceWM active title bar
    # is blue on this stock image; inspect only runs wide enough for this dialog.
    for y in range(0,frame.height-10):
        runs=[];start=None
        for x in range(frame.width):
            r,g,b=frame.getpixel((x,y))[:3]
            active=b>100 and b>r*1.5 and b>g*1.5
            if active and start is None:start=x
            if not active and start is not None:
                if x-start>=width-100:runs.append((start,x))
                start=None
        if runs:
            left,right=max(runs,key=lambda run:run[1]-run[0])
            # First title text scanline varies; find bottom of blue title band.
            yy=y
            while yy+1<frame.height and any(frame.getpixel((xx,yy+1))[:3]==frame.getpixel((left+2,y))[:3] for xx in [left+2,left+10]):yy+=1
            return left,yy+2
    raise AssertionError('Active modal title rectangle not found')

def wait_export(s,total,timeout=120):
    expected=rf'(?:{total}.*completed|已完成{total}项).*?(?:0.*failed|失败0项)'
    text=s.wait_text(expected,'export-results',timeout)
    return text

def wizard(s,button,values=(),tag=None):
    s.click(273,17);s.click(*button);wait_focus(s,False)
    frame=s.frame(tag or 'wizard')
    x,y=dialog_origin(frame,820,600)
    s.probe.click(x+602,y+565);wait_focus(s,True)
    for n,value in enumerate(values):
        if n:s.keys('tab')
        s.probe.text(value)
    time.sleep(1)

def exercise_tools(s):
    original=r'x=\frac{-b\pm\sqrt{b^2-4ac}}{2a}'
    s.editor_source('');wizard(s,(55,55),['n+1','n'],'fraction-wizard')
    assert source_clipboard(s)==r'\frac{n+1}{n}'
    s.editor_source('');wizard(s,(55,85),['x+1'],'root-wizard')
    assert source_clipboard(s)==r'\sqrt{x+1}'
    s.editor_source('');wizard(s,(285,55),['0','1','x^2','dx'],'integral-wizard')
    assert source_clipboard(s)==r'\int_{0}^{1} x^2 \, dx'
    for point,environment in [((520,55),'matrix'),((520,85),'cases'),((630,55),'aligned')]:
        s.editor_source('');wizard(s,point,['u','v'],environment+'-wizard')
        text=source_clipboard(s)
        assert r'\begin{'+environment+'}' in text and 'u' in text and 'v' in text
    s.editor_source(r'\alph');s.keys('ctrl-spc');wait_focus(s,False)
    x,y=dialog_origin(s.probe.frame('completion-dialog'),860,560)
    s.probe.click(x+650,y+527);wait_focus(s,True)
    assert source_clipboard(s)==r'\alpha'
    s.editor_source('x+x');s.keys('ctrl-h');wait_focus(s,False)
    x,y=dialog_origin(s.probe.frame('replace-dialog'),720,370)
    s.probe.click(x+290,y+35);s.fill('x');s.probe.click(x+290,y+85);s.fill('y')
    s.probe.click(x+365,y+332);s.probe.click(x+635,y+332);wait_focus(s,True)
    assert source_clipboard(s)=='y+y'
    s.editor_source(original);time.sleep(2)

def exercise(s,recovery=False):
    evidence={'checks':[],'captures':[]}
    deadline=time.monotonic()+60
    while True:
        frame=s.frame('reliefmath-editor')
        text=fltk.tesseract_ocr(frame,s.output,'reliefmath-chinese',psm=11)
        if fltk.cjk_count(text)>=25:break
        if time.monotonic()>=deadline:raise AssertionError('Chinese UI labels were not rendered')
        time.sleep(.5)
    evidence['initial_geometry']=list(s.geometry)
    for n in range(5):s.click(117+78*n,17);s.screenshot('ribbon-'+str(n))
    original=s.geometry
    resize(s,960,600);s.screenshot('reliefmath-minimum')
    resize(s,original[2],original[3]);s.screenshot('reliefmath-restored')
    evidence['checks']+=['five-ribbon-pages','minimum-and-restored-window']
    s.click(429,17);s.click(239,55);s.screenshot('reliefmath-list-hidden');s.click(239,55)
    s.click(239,85);s.screenshot('reliefmath-preview-hidden');s.click(239,85)
    s.click(110,55);s.screenshot('reliefmath-zoom');s.click(40,55)
    evidence['checks']+=['toggle-list-and-preview','preview-zoom']
    s.click(117,17);s.click(300,210);s.keys('ctrl-a');s.probe.text('x=1');s.wait_text('预览.*(?:最新|更新|就绪)|Preview.*up','preview-current')
    s.keys('ctrl-z');time.sleep(.5)
    assert source_clipboard(s)==r'x=\frac{-b\pm\sqrt{b^2-4ac}}{2a}'
    s.click(300,210);s.keys('ctrl-y');time.sleep(.4)
    assert source_clipboard(s)=='x=1'
    s.keys('ctrl-z');time.sleep(.3)
    exercise_tools(s)
    evidence['checks']+=['fraction-root-integral-tab-placeholders','matrix-cases-aligned-tab-placeholders','command-completion','find-replace']
    s.keys('ctrl-shift-s');filename(s,'/home/test/saved.rmath','save-native')
    s.wait_text('文档已保存|Document.*saved','saved-status')
    evidence['checks']+=['global-undo-redo','native-save-as']
    # Exercise a structure wizard, editing its live template before insertion.
    select_formula(s,3);time.sleep(2);s.click(273,17);s.click(520,55);wait_focus(s,False);s.screenshot('reliefmath-structure')
    x,y=dialog_origin(s.probe.frame('structure-options'),820,600)
    s.probe.click(x+740,y+565);wait_focus(s,True);select_formula(s,1);time.sleep(2)
    # A bad command must retain the prior image and report the error.
    s.editor_source(r'\unsupported{x}');s.wait_text('旧预览.*(?:错误|出错)|Oldpreview.*error','reliefmath-error-recovery')
    s.click(s.geometry[2]-60,156);s.screenshot('reliefmath-error-location')
    s.keys('ctrl-z');s.wait_text('预览.*(?:最新|更新|就绪)|Preview.*up','reliefmath-error-fixed')
    s.click(117,17);s.click(816,83)
    xorg.wait_serial_after(s.serial,'RM_CLIPBOARD_IMAGE_PASS',s.process,s.launch_offset,timeout=30)
    evidence['checks']+=['error-keeps-old-preview','error-locate-and-recover','independent-image-clipboard']
    for fmt in [0,1,2]:
        start_export(s,fmt);wait_export(s,5);s.screenshot('reliefmath-export' if fmt==1 else 'export-format-'+str(fmt))
        close_export(s)
    evidence['checks']+=['svg-export','transparent-png-export','latex-source-export']
    s.keys('ctrl-o');filename(s,'/home/test/broken.rmath','open-broken')
    time.sleep(1);assert source_clipboard(s)==r'x=\frac{-b\pm\sqrt{b^2-4ac}}{2a}'
    s.screenshot('reliefmath-broken-file-kept');evidence['checks']+=['broken-open-keeps-current-document']
    close_app(s)
    s.launch('/home/test/partial.rmath')
    start_export(s,0,pattern='partial-{index}')
    s.wait_text('已完成1项.*失败1项|1completed.*1failed','reliefmath-partial-export',timeout=120)
    s.screenshot('reliefmath-partial-export');close_export(s);close_app(s)
    evidence['checks']+=['per-item-render-failure-continues']
    s.launch('/home/test/custom-size.rmath');s.click(351,17)
    s.click(70,85);s.keys('end','ret');time.sleep(.5)
    s.screenshot('reliefmath-custom-size');s.keys('ctrl-shift-s')
    filename(s,'/home/test/custom-size-saved.rmath','save-custom-size');time.sleep(1);close_app(s)
    evidence['checks']+=['display-style-preserves-nonpreset-30pt']
    s.launch('/home/test/acceptance.rmath',worker_fail_after=20)
    xorg.wait_serial_after(s.serial,'RM_WORKER_KILL',s.process,s.launch_offset,timeout=35)
    s.click(429,17);s.screenshot('reliefmath-worker-failure-responsive')
    s.editor_source('x^2+1');s.wait_text('预览.*(?:最新|更新|就绪)|Preview.*up','reliefmath-worker-restarted')
    s.editor_source(r'x=\frac{-b\pm\sqrt{b^2-4ac}}{2a}');time.sleep(1);close_app(s)
    evidence['checks']+=['renderer-worker-sigkill-and-controlled-restart','ui-responsive-after-worker-failure']
    if recovery:
        s.launch('/home/test/saved.rmath',crash_after=90)
        s.editor_source(r'\text{Recovery marker 2468}+x^2')
        started=time.monotonic();print('Waiting for real 60-second recovery timer',flush=True)
        s.wait_text('恢复快照已保存|Recoverysnapshotsaved','reliefmath-recovery-snapshot',timeout=85)
        evidence['recovery_wait_seconds']=time.monotonic()-started
        xorg.wait_serial_after(s.serial,'RM_EXIT status=137',s.process,s.launch_offset,timeout=50)
        s.launch('');s.screenshot('reliefmath-recovery-offer')
        # Recovery dialog is 650x320; first button recovers into an untitled document.
        x,y=dialog_origin(s.probe.frame('recovery-modal'),650,320)
        s.probe.click(x+110,y+282);wait_focus(s,True)
        assert source_clipboard(s)==r'\text{Recovery marker 2468}+x^2'
        s.keys('ctrl-shift-s');filename(s,'/home/test/recovered.rmath','save-recovered');time.sleep(1)
        close_app(s);evidence['checks']+=['actual-timer-crash-recovery-as-new']
    for name in ['reliefmath-editor','reliefmath-structure','reliefmath-error-recovery','reliefmath-export']:
        path=s.output/(name+'.png');assert path.is_file(),'Missing required screenshot: '+name
        evidence['captures'].append({'file':path.name,'sha256':fltk.sha256(path)})
    (s.output/'gui-evidence.json').write_text(json.dumps(evidence,ensure_ascii=False,indent=2)+'\n')
    return evidence

def verify_disk(output,metadata,recovery):
    from PIL import Image
    import xml.etree.ElementTree as ET
    partitions=metadata['gpt_partitions'];part=next(p for p in partitions if p['name']=='RELIEFOS_ROOT')
    root=output/'verified-root.ext4'
    # Sparse extraction avoids allocating the filesystem's unused blocks.
    subprocess.run(['dd','if='+str(output/'guest-private.raw'),'of='+str(root),'bs=512',
                    'skip='+str(part['first']),'count='+str(part['last']-part['first']+1),
                    'conv=sparse','status=none'],check=True)
    readback=output/'readback';readback.mkdir()
    files=['saved.rmath','custom-size-saved.rmath','clipboard.ppm','partial-002.svg']+[f'{n:03d}.{ext}' for ext in ['svg','png','tex'] for n in range(1,6)]
    if recovery:files.append('recovered.rmath')
    entries=[]
    try:
        for name in files:
            path=readback/name
            subprocess.run(['debugfs','-R',f'dump /home/test/{name} {path}',str(root)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            if not path.is_file():raise AssertionError('Missing persisted guest output: '+name)
            entries.append({'path':name,'sha256':fltk.sha256(path),'bytes':path.stat().st_size})
        for item in metadata['injected_files']:
            if item['guest_path'] in ['/usr/lib/reliefos/apps/reliefmath/reliefmath.elf','/usr/lib/reliefos/apps/reliefmath/reliefmath-render.elf','/usr/share/reliefmath/STIXTwoMath-Regular.otf']:
                path=readback/Path(item['guest_path']).name
                subprocess.run(['debugfs','-R',f'dump {item["guest_path"]} {path}',str(root)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
                assert fltk.sha256(path)==item['sha256'],'Guest changed executable/font'
        subprocess.run([str(output/'guest-verify'),'verify',str(readback)],check=True)
        subprocess.run([str(output/'guest-verify'),'verify-style',str(readback)],check=True)
        if recovery:subprocess.run([str(output/'guest-verify'),'verify-recovery',str(readback)],check=True)
        for n in range(1,6):
            svg=ET.parse(readback/f'{n:03d}.svg').getroot();paths=0
            for node in svg.iter():
                assert node.tag.split('}')[-1] in {'svg','g','path'}
                assert not any(k.lower().startswith('on') or k.endswith('href') or 'url(' in v.lower() for k,v in node.attrib.items())
                paths+=node.tag.endswith('path') and bool(node.attrib.get('d'))
            assert paths
            png=Image.open(readback/f'{n:03d}.png').convert('RGBA')
            assert png.getchannel('A').getextrema()[0]==0 and png.getchannel('A').getextrema()[1]>0
            assert png.size==(int(svg.attrib['width']),int(svg.attrib['height']))
        clipboard=Image.open(readback/'clipboard.ppm').convert('RGB')
        png=Image.open(readback/'001.png').convert('RGBA')
        expected=Image.new('RGB',png.size,'white');expected.paste(png,mask=png.getchannel('A'))
        assert clipboard.size==expected.size
        from PIL import ImageChops
        # FLTK and PNG alpha conversion may round individual channel values by 1.
        difference=ImageChops.difference(clipboard,expected)
        assert all(high<=2 for low,high in difference.getextrema()),'Clipboard image differs from actual export'
        (output/'persisted-verification.json').write_text(json.dumps({'outputs':entries,'native':True,'svg_paths':True,'transparent_png':True,'clipboard_matches_export':True,'executable_font_hashes':True},indent=2)+'\n')
    finally:root.unlink(missing_ok=True)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--image',type=Path,required=True);p.add_argument('--kernel',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--recovery',action='store_true');p.add_argument('--interactive',action='store_true',help='development only; does not certify acceptance')
    args=p.parse_args();output=args.output.resolve();metadata=prepare(args.image.resolve(),args.kernel.resolve(),output);s=None
    try:
        s=Session(output);s.login();s.launch();s.screenshot('reliefmath-initial')
        if args.interactive:code.interact(local={'s':s,'metadata':metadata,'args':args,'output':output,'time':time,'fltk':fltk,'xorg':xorg})
        else:exercise(s,args.recovery)
    finally:
        if s:s.close()
    if fltk.sha256(args.image)!=metadata['source_image_sha256_before']:raise AssertionError('Original image changed')
    if args.interactive:print('Development session finished; full acceptance not certified')
    else:
        verify_disk(output,metadata,args.recovery)
        (output/'acceptance.json').write_text(json.dumps({'passed':True,'source_image_unchanged':True,'platform':'QEMU KVM / ReliefOS Xorg','vmware':'not verified','hardware':'not verified'},indent=2)+'\n')
        print('ReliefMath QEMU GUI and independent disk verification passed',flush=True)

if __name__=='__main__':main()
