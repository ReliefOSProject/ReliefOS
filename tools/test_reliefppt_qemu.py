#!/usr/bin/env python3
"""Exercise ReliefPPT in a private QEMU image, then independently inspect saved files."""
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
    core=[ROOT/'userland/apps/reliefppt'/name for name in ('model.cpp','document.cpp','geometry.cpp')]
    with (output/'host-helper-build.log').open('w') as log:
        subprocess.run([os.environ.get('HOSTCXX','c++'),'-std=c++17','-O2','-Wall','-Wextra','-Werror','-fno-exceptions','-fno-rtti',
                        '-I'+str(ROOT/'userland/apps/reliefppt'),str(ROOT/'tools/tests/reliefppt_guest_verify.cpp'),
                        *map(str,core),'-lm','-o',str(output/'guest-verify')],cwd=ROOT,env=task_env,stdout=log,stderr=log,check=True)
    subprocess.run([str(output/'guest-verify'),'prepare',str(fixtures)],cwd=ROOT,env=task_env,check=True)
    from PIL import Image
    rgba=Image.new('RGBA',(32,24),(40,130,210,255))
    for y in range(24):
        for x in range(4): rgba.putpixel((x,y),(40,130,210,0))
    rgba.save(fixtures/'import-picture.png')
    Image.new('RGB',(16,12),(220,100,20)).save(fixtures/'import-picture.bmp')
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
    runner=output/'rp-runner'
    runner.write_text('''#!/bin/sh
export LC_ALL=zh_CN.UTF-8 LANG=zh_CN.UTF-8
/usr/lib/reliefos/tests/reliefppt-clipboard >/dev/ttyS0 2>&1 &
clipboard_pid=$!
sleep 1
/usr/lib/reliefos/apps/reliefppt/reliefppt.elf "$@" >/dev/ttyS0 2>&1 &
pid=$!
echo "RP_START pid=$pid" >/dev/ttyS0
crash_pid=
if [ -n "$RP_CRASH_AFTER" ]; then
    (sleep "$RP_CRASH_AFTER"; kill -KILL "$pid") &
    crash_pid=$!
fi
/usr/lib/reliefos/tests/fltk-ui-probe --watch-window-focus "$pid" >/dev/ttyS0 2>&1 &
probe_pid=$!
/usr/lib/reliefos/tests/reliefppt-icon-probe "$pid" reliefppt >/dev/ttyS0 2>&1
echo "RP_ICON_STATUS status=$?" >/dev/ttyS0
wait "$pid"
status=$?
kill "$clipboard_pid" "$probe_pid"
if [ -n "$crash_pid" ]; then kill "$crash_pid" 2>/dev/null; fi
echo "RP_EXIT status=$status" >/dev/ttyS0
''')
    entry=output/'reliefppt-entry'
    entry.write_text('#!/bin/sh\nexec /usr/lib/reliefos/apps/reliefppt/reliefppt.elf "$@"\n')
    for app in ('reliefppt','fileman'):
        fltk.validate_target_elf(OUT/f'userland/{app}.elf',sysroot=OUT/'sysroot/musl',required_needed=('libfltk.so.1.3',))
    payload=[
        (OUT/'userland/reliefppt.elf','/usr/lib/reliefos/apps/reliefppt/reliefppt.elf',0o100755),
        (OUT/'userland/fileman.elf','/usr/lib/reliefos/apps/fileman/fileman.elf',0o100755),
        (OUT/'generated/nls/zh_CN/LC_MESSAGES/leonos.mo',fltk.LOCALE_GUEST,0o100644),
        (ROOT/'userland/apps/reliefppt/reliefppt.desktop','/usr/share/applications/reliefppt.desktop',0o100644),
        (ROOT/'resources/build-art/app-icons/reliefppt.bmp','/usr/lib/reliefos/apps/reliefppt/reliefppt.bmp',0o100644),
        (entry,'/usr/bin/reliefppt',0o100755),(runner,'/usr/bin/rp',0o100755),
        (probe,fltk.PROBE_GUEST,0o100755),
        (output/'clipboard.elf','/usr/lib/reliefos/tests/reliefppt-clipboard',0o100755),
        (output/'icon-probe.elf','/usr/lib/reliefos/tests/reliefppt-icon-probe',0o100755),
    ]
    for size in (16,32,48,64,128,256):
        path=ROOT/f'resources/build-art/app-icons/hicolor/{size}x{size}/apps/reliefos-reliefppt.png'
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
        prefix=f'RP_CRASH_AFTER={crash_after} ' if crash_after else ''
        self.probe.text(prefix+'rp'+(' '+argument if argument else '')); self.probe.key('ret')
        deadline=time.monotonic()+60
        prompted=False
        while time.monotonic()<deadline:
            log=self.serial.read_text(errors='replace')[offset:]
            match=re.search(r'RP_START pid=(\d+)',log)
            if match and not prompted and (recover or import_warning):
                time.sleep(2)
                self.probe.frame('reliefppt-startup-prompt')
                # Recover is the native dialog's default Return button.
                if recover: self.keys('ret')
                else: self.keys('ret')
                prompted=True
            if match and '[reliefppt.elf] FLTK presentation editor ready' in log:
                self.pid=int(match[1]); self.geometry=fltk.wait_client_geometry(self.serial,self.process,self.pid,offset)
                xorg.wait_serial_after(self.serial,'APP_ICON_PASS app=reliefppt ',self.process,offset,timeout=40)
                wait_editor_focus(self)
                return self.geometry
            if self.process.poll() is not None: raise RuntimeError('QEMU exited')
            time.sleep(.3)
        raise AssertionError('ReliefPPT did not become ready')

    def frame(self,name):
        frame=super().frame(name)
        if 'RP_EXIT status=' in self.serial.read_text(errors='replace')[getattr(self,'launch_offset',0):]:
            raise AssertionError('ReliefPPT exited before screenshot '+name)
        return frame

    def keys(self,*keys):
        for key in keys: self.probe.key(key)

    def fill(self,value):
        self.keys('home','shift-end','backspace'); self.probe.text(value)


def export_document(session,format,name):
    session.click(36,17)
    session.probe.frame('export-'+format+'-menu')
    session.click(70,41+19*(5 if format=='HTML' else 4))
    choose_filename(session,name,'export-'+format+'-chooser')
    session.frame('reliefppt-exported-'+format)


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
        if 'RP_EXIT status=' in log: raise AssertionError('ReliefPPT exited while waiting for focus')
        time.sleep(.05)
    session.probe.frame('focus-timeout-'+('editor' if focused else 'dialog'))
    raise AssertionError(f'ReliefPPT focus did not settle: pid={session.pid} focused={focused}')


def editor_focus_after(session,*keys):
    offset=len(session.serial.read_text(errors='replace'))
    session.keys(*keys)
    wait_editor_focus(session,after=offset)


def close_document(session):
    offset=len(session.serial.read_text(errors='replace'))
    session.keys('alt-f4')
    xorg.wait_serial_after(session.serial,'RP_EXIT status=0',session.process,offset,timeout=20)
    time.sleep(.4)


def canvas_transform(session):
    """Mirror the published fit/paper coordinates, without reading app memory."""
    x,y,w,h=session.geometry
    cw,ch=w-192,h-164
    scale=max(.25,min(2,int(min((cw-54)/960,(ch-54)/540)*100)/100))
    pw,ph=round(960*scale),round(540*scale)
    px=192+((cw-14-pw)//2 if cw-14>pw+40 else 20)
    py=136+((ch-14-ph)//2 if ch-14>ph+40 else 20)
    return x+px,y+py,scale


def slide_point(session,x,y):
    px,py,scale=canvas_transform(session)
    return round(px+x*scale),round(py+y*scale)


def click_slide(session,x,y):
    session.probe.click(*slide_point(session,x,y))


def drag_slide(session,x,y,dx,dy):
    start=slide_point(session,x,y); end=slide_point(session,x+dx,y+dy)
    session.probe.move(*start); session.probe.button(True); time.sleep(.15)
    session.probe.move(*end); time.sleep(.25); session.probe.button(False); time.sleep(.3)


def edit_title(session,text=None):
    click_slide(session,240,95); session.keys('f2','ctrl-end','ret')
    if text is None:
        session.keys('ctrl-v','ret'); session.probe.text('PptCheck: ')
        for digit in '123456789':
            session.probe.command('human-monitor-command',{'command-line':f'sendkey {digit} 20'})
            time.sleep(.04)
    else:
        session.probe.text(text)


def verify_edit_overlay(session,frame):
    px,py,scale=canvas_transform(session)
    left=px+int(72*scale)+1; top=py+int(52*scale)+1
    right=left+int(max(1,816*scale-2))-1
    bottom=top+int(max(1,112*scale-2))-1
    rgb=frame.convert('RGB')
    # The input box has a grey vertical outline. Check its far edge away from
    # glyphs; checking both sizes catches an input left at its old geometry.
    ratios=[]
    for x in (left,right):
        hits=0
        for y in range(top+5,bottom-4):
            nearby=[rgb.getpixel((xx,y)) for xx in range(x-2,x+3)]
            hits+=any(max(p)-min(p)<18 and 45<sum(p)/3<210 for p in nearby)
        ratios.append(hits/max(1,bottom-top-9))
    if min(ratios)<.65: raise AssertionError('Text edit overlay did not follow resized slide: '+str(ratios))


def exercise(s):
    s.launch('/home/test/ppt-fixture.rppt')
    frame=s.frame('reliefppt-document')
    visible=fltk.tesseract_ocr(frame,s.output,'ppt-chinese',psm=11)
    if fltk.cjk_count(visible)<35: raise AssertionError('Chinese slide/UI text was not visible')
    print('ReliefPPT ready; Chinese document and ribbon captured',flush=True)
    edit_title(s)
    original=s.geometry
    for width,height,tag in [(900,600,'minimum'),(original[2],original[3],'restored')]:
        x,y,w,h=s.geometry; offset=len(s.serial.read_text(errors='replace'))
        s.probe.move(x+w+2,y+h+2); s.probe.button(True); time.sleep(.15)
        s.probe.move(x+width+2,y+height+2); time.sleep(.25); s.probe.button(False)
        s.geometry=fltk.wait_client_geometry(s.serial,s.process,s.pid,offset,width,height)
        frame=s.frame('reliefppt-'+tag)
        verify_edit_overlay(s,frame)
        x,y,w,h=s.geometry
        status=fltk.tesseract_ocr(frame.crop((x+w-51,y+h-25,x+w-8,y+h-3)),s.output,
                                  'ppt-status-'+tag,psm=7,language='eng',scale=4)
        expected=round(canvas_transform(s)[2]*100)
        if str(expected) not in status: raise AssertionError('Zoom status clipped after resize: '+status)
    s.keys('esc')
    s.frame('reliefppt-edit-finished')
    s.click(570,55); s.keys('ctrl-a'); s.probe.text('36'); s.keys('ret')
    s.frame('reliefppt-size-edited')
    s.click(775,85)
    s.frame('reliefppt-properties-opened')
    wait_editor_focus(s,focused=False)
    frame=s.probe.frame('reliefppt-object-properties'); sw,sh=frame.size
    dx=(sw-470)//2; dy=(sh-30-430)//2
    s.probe.click(dx+350,dy+126); s.keys('ctrl-a'); s.probe.text('180')
    offset=len(s.serial.read_text(errors='replace'))
    s.probe.click(dx+400,dy+395); wait_editor_focus(s,after=offset)
    click_slide(s,790,300); time.sleep(.5)
    drag_slide(s,790,300,40,20)
    drag_slide(s,904,386,30,20)
    # Exercise actual object clipboard and undo/redo, finishing at the original
    # object count so exact native readback can detect a duplicate left behind.
    s.keys('ctrl-c','ctrl-v','ctrl-z','ctrl-y','ctrl-z')
    s.frame('reliefppt-edited-objects')
    s.click(208,85)
    s.click(296,17); s.click(900,85)
    s.click(80,175); s.click(120,17)
    s.keys('ctrl-s'); time.sleep(.6)
    s.frame('reliefppt-edited')
    export_document(s,'PNG','acceptance.png')
    export_document(s,'HTML','acceptance.html')
    s.keys('f5'); time.sleep(.6); first=s.frame('reliefppt-slideshow-first')
    s.keys('right'); second=s.frame('reliefppt-slideshow-second')
    second_text=fltk.tesseract_ocr(second,s.output,'ppt-sans-cjk',psm=11)
    if '第一步' not in re.sub(r'\s+','',second_text): raise AssertionError('Sans-serif CJK fallback failed')
    s.keys('b'); black=s.frame('reliefppt-slideshow-black')
    # Read real fullscreen pixels; a no-op B key must fail this check.
    center=black.crop((black.width//4,black.height//4,black.width*3//4,black.height*3//4)).convert('RGB')
    if any(max(channel)>3 for channel in center.getextrema()): raise AssertionError('B did not black the slide show')
    s.keys('b','end'); last=s.frame('reliefppt-slideshow-last')
    last_text=fltk.tesseract_ocr(last,s.output,'ppt-mono-cjk',psm=11)
    if '谢谢观看' not in re.sub(r'\s+','',last_text): raise AssertionError('Monospace CJK fallback failed')
    s.keys('right'); s.frame('reliefppt-slideshow-end')
    editor_focus_after(s,'esc'); s.click(80,175)
    close_document(s)
    s.launch('/home/test/ppt-fixture.rppt'); s.frame('reliefppt-reopened')
    # Shift+F5 must start from the clicked second slide, not the first.
    s.click(80,290); s.click(386,17); s.click(500,55)
    s.frame('reliefppt-speaker-notes'); s.click(500,55)
    s.keys('shift-f5'); frame=s.frame('reliefppt-slideshow-current')
    text=fltk.tesseract_ocr(frame,s.output,'ppt-current',psm=11)
    if '第一步' not in re.sub(r'\s+','',text): raise AssertionError('Shift+F5 did not start at the selected slide')
    editor_focus_after(s,'esc'); close_document(s)
    return [('ppt-fixture.rppt','edited'),('acceptance.png','png'),('acceptance.html','html')]


def exercise_branch(s):
    s.launch('/home/test/ppt-fixture.rppt'); s.keys('ctrl-n')
    s.click(885,55); s.probe.text('A'); s.keys('esc','ctrl-s')
    choose_filename(s,'branch-a.rppt','branch-native-chooser')
    s.keys('ctrl-z','f2','ctrl-a'); s.probe.text('B'); s.keys('esc','alt-f4')
    wait_editor_focus(s,focused=False); s.probe.frame('reliefppt-branch-close-prompt')
    editor_focus_after(s,'esc'); s.frame('reliefppt-branch-still-open')
    s.keys('ctrl-shift-s'); choose_filename(s,'branch.rppt','branch-b-native-chooser')
    close_document(s)
    return [('branch-a.rppt','branch-a'),('branch.rppt','branch')]


def exercise_images(s):
    s.launch(); s.click(885,85)
    # Start in the user's home directory. Typing an initial slash one key at a
    # time makes the upstream chooser change directories before the path is
    # complete; use its normal relative-name entry instead.
    choose_filename(s,'import-picture.png','png-import-chooser')
    s.frame('reliefppt-png-import')
    s.click(885,85); choose_filename(s,'import-picture.bmp','bmp-import-chooser')
    s.keys('ctrl-s'); choose_filename(s,'images.rppt','images-native-chooser')
    close_document(s)
    s.launch('/home/test/overflow-fixture.rppt'); s.frame('reliefppt-text-overflow')
    export_document(s,'PNG','overflow.png'); close_document(s)
    return [('images.rppt','images'),('overflow.png','overflow')]


def exercise_recovery(s):
    s.launch('/home/test/ppt-fixture.rppt',crash_after=85)
    edit_title(s,'Recovery marker 2468')
    def wait_for_real_autosave_and_crash(generation, frame_tag):
        print(f'Recovery generation {generation}: waiting for the real 60-second autosave',flush=True)
        deadline=time.monotonic()+62
        while time.monotonic()<deadline:
            if s.process.poll() is not None: raise RuntimeError('QEMU exited during autosave')
            time.sleep(min(.5,deadline-time.monotonic()) if deadline>time.monotonic() else 0)
        s.frame(frame_tag)
        xorg.wait_serial_after(s.serial,'RP_EXIT status=137',s.process,s.launch_offset,timeout=35)
        time.sleep(.5)

    wait_for_real_autosave_and_crash(1,'reliefppt-recovery-copy')
    s.launch('/home/test/ppt-fixture.rppt',recover=True,crash_after=85)
    s.frame('reliefppt-recovered-first-generation')
    edit_title(s,'Recovery marker 8642')
    wait_for_real_autosave_and_crash(2,'reliefppt-recovery-copy-second-generation')
    s.launch('/home/test/ppt-fixture.rppt',recover=True)
    s.frame('reliefppt-recovered-second-generation')
    s.keys('ctrl-s'); choose_filename(s,'recovered.rppt','recovery-native-chooser')
    close_document(s)
    return [('recovered.rppt','recovery')]


def verify_export(path,mode,output):
    import base64
    import io
    from PIL import Image, ImageStat
    if mode in ('png','overflow'):
        image=Image.open(path); image.load()
        if image.size!=(960,540): raise AssertionError('PNG dimensions do not match slide units')
        if max(ImageStat.Stat(image.convert('RGB')).stddev)<10: raise AssertionError('PNG is blank')
        if mode=='overflow':
            if image.convert('RGB').getpixel((650,160))!=(224,90,64):
                raise AssertionError('Overflowing text prevented later slide objects from rendering')
            return
        ocr=fltk.tesseract_ocr(image,output,'exported-png',psm=11)
        if 'PptCheck' not in ocr or fltk.cjk_count(ocr)<20: raise AssertionError('PNG omits edited Chinese slide text')
        return
    source=path.read_text()
    if '这段备注' in source or 'Recovery marker' in source: raise AssertionError('Private notes leaked into HTML')
    payloads=re.findall(r'data:image/png;base64,([A-Za-z0-9+/=]+)',source)
    if len(payloads)!=4: raise AssertionError('HTML did not contain four embedded slide images')
    images=[]
    for payload in payloads:
        image=Image.open(io.BytesIO(base64.b64decode(payload,validate=True))); image.load()
        if image.size!=(960,540): raise AssertionError('HTML slide dimensions wrong')
        images.append(image)
    with Image.open(output/'extracted/acceptance.png') as png:
        if images[0].convert('RGB').tobytes()!=png.convert('RGB').tobytes():
            raise AssertionError('HTML first slide differs from the same renderer PNG')
    if re.search(r'(?:src|href)\s*=\s*[\"\'](?:https?:|//)',source,re.I):
        raise AssertionError('HTML depends on a network resource')
    if 'ArrowRight' not in source or 'Home' not in source or 'End' not in source: raise AssertionError('HTML lacks navigation controls')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image',required=True,type=Path)
    parser.add_argument('--kernel',required=True,type=Path)
    parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--recovery',action='store_true')
    args=parser.parse_args(); output=args.output.resolve()
    metadata=prepare(args.image.resolve(),args.kernel.resolve(),output)
    session=None
    try:
        session=Session(output)
        wait_log(session.serial,'boot complete:',session.process,120)
        xorg.wait_greeter_stage(session.probe,session.process,'login','ppt-greeter',timeout=120)
        session.login(); expected=exercise(session)+exercise_branch(session)+exercise_images(session)
        if args.recovery: expected+=exercise_recovery(session)
    finally:
        if session is not None: session.close()
    extracted=output/'extracted'; extracted.mkdir()
    part=next(p for p in metadata['gpt_partitions'] if p['name']=='RELIEFOS_ROOT')
    root=vdso.extract_root(output/'guest-private.raw',{'root_first':int(part['first']),'root_last':int(part['last'])},extracted)
    for name,mode in expected:
        destination=extracted/name
        subprocess.run(['debugfs','-R',f'dump /home/test/{name} {fltk.debugfs_quote(destination)}',str(root)],check=True)
        if mode in ('png','html','overflow'): verify_export(destination,mode,output)
        else: subprocess.run([str(output/'guest-verify'),str(destination),mode],check=True)
    if fltk.sha256(args.image)!=metadata['source_image_sha256_before']: raise AssertionError('Source image changed')
    payload=next(row for row in metadata['injected_files'] if row['guest_path']=='/usr/lib/reliefos/apps/reliefppt/reliefppt.elf')
    if fltk.sha256(OUT/'userland/reliefppt.elf')!=payload['sha256']: raise AssertionError('Target ELF changed during validation')
    result={'passed':True,'verified':expected,'source_image_unchanged':True,'vmware_verified':False,
            'target_elf_sha256':payload['sha256']}
    (output/'result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
    print(json.dumps(result,ensure_ascii=False,indent=2),flush=True)


if __name__=='__main__': main()
