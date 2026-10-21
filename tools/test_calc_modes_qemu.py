#!/usr/bin/env python3
"""Exercise all calculator modes and capture real pixels in a private guest.

The kernel is an explicit input. This test never treats an older kernel as proof
that a newer kernel source checkout builds, and never edits the source disk.
"""
from pathlib import Path
import argparse
import code
import json
import os
import re
import subprocess
import time
import traceback
import test_fltk_apps_qemu as fltk
import test_vdso_qemu as vdso
import test_xorg_vm_qemu as xorg
from test_reliefmath_qemu import Session as CommonSession, resize, wait_focus
from qmp_terminal_smoke import text_keys

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'out/xorg-vmware-debug'

def prepare(image,kernel,output):
    output.mkdir(parents=True,exist_ok=False)
    probe=fltk.target_compile_probe(output,OUT/'sysroot/musl','clang++','ld.lld')
    template=json.loads((output/'probe-build.command.json').read_text())
    command=list(template['compile']); command[-3]=str(ROOT/'tools/tests/calc_clipboard.cpp'); command[-1]=str(output/'clipboard.o')
    subprocess.run(command,cwd=ROOT,check=True,capture_output=True)
    command=list(template['link']); command[command.index(str(output/'fltk_ui_probe.o'))]=str(output/'clipboard.o')
    command[command.index(str(probe))]=str(output/'clipboard.elf')
    subprocess.run(command,cwd=ROOT,check=True,capture_output=True)
    objects=[]
    sources=[ROOT/'tests/host/test_calc_modes.cpp']+[ROOT/'userland/apps/calc'/name for name in
              ('model.cpp','programmer.cpp','units.cpp','utilities.cpp')]
    with (output/'core-build.log').open('w') as log:
        for source in sources:
            obj=output/('core-'+source.stem+'.o'); command=list(template['compile'])
            command[-3],command[-1]=str(source),str(obj)
            command.insert(1,'-iquote'+str(ROOT/'userland/apps/calc'))
            subprocess.run(command,cwd=ROOT,stdout=log,stderr=log,check=True); objects.append(str(obj))
        command=list(template['link']); index=command.index(str(output/'fltk_ui_probe.o'))
        command[index:index+1]=objects
        command[command.index(str(probe))]=str(output/'calc-core.elf')
        subprocess.run(command,cwd=ROOT,stdout=log,stderr=log,check=True)
    runner=output/'runner'
    runner.write_text('''#!/bin/sh
export LC_ALL=zh_CN.UTF-8 LANG=zh_CN.UTF-8
if ! /usr/lib/reliefos/tests/calc-core >/dev/ttyS0 2>&1; then
 echo CALC_CORE_FAIL >/dev/ttyS0
 exit 1
fi
echo CALC_CORE_PASS >/dev/ttyS0
/usr/lib/reliefos/tests/calc-clipboard >/dev/ttyS0 2>&1 &
clipboard=$!
sleep 1
/usr/lib/reliefos/apps/calc/calc.elf >/dev/ttyS0 2>&1 &
pid=$!
echo "CALC_START pid=$pid" >/dev/ttyS0
/usr/lib/reliefos/tests/fltk-ui-probe --watch-window-focus "$pid" >/dev/ttyS0 2>&1 &
watcher=$!
wait "$pid"
status=$?
kill "$watcher" 2>/dev/null
kill "$clipboard" 2>/dev/null
echo "CALC_EXIT status=$status" >/dev/ttyS0
''')
    elf=OUT/'userland/calc.elf'
    fltk.validate_target_elf(elf,sysroot=OUT/'sysroot/musl',required_needed=('libfltk.so.1.3',))
    payload=[(elf,'/usr/lib/reliefos/apps/calc/calc.elf',0o100755),
             (OUT/'generated/nls/zh_CN/LC_MESSAGES/leonos.mo',fltk.LOCALE_GUEST,0o100644),
             (OUT/'system/lib/libreliefos.so.2','/usr/lib/reliefos/libreliefos.so.2',0o100755),
             (runner,'/usr/bin/calctest',0o100755),(probe,fltk.PROBE_GUEST,0o100755),
             (output/'calc-core.elf','/usr/lib/reliefos/tests/calc-core',0o100755),
             (output/'clipboard.elf','/usr/lib/reliefos/tests/calc-clipboard',0o100755),
             (ROOT/'userland/apps/calc/calc.desktop','/usr/share/applications/reliefos-calc.desktop',0o100644)]
    original=vdso.extract_root
    def sparse(raw,layout,folder):
        root=folder/'guest-root.ext4'
        subprocess.run(['dd','if='+str(raw),'of='+str(root),'bs=512','skip='+str(layout['root_first']),
                        'count='+str(int(layout['root_last'])-int(layout['root_first'])+1),'conv=sparse','status=none'],check=True)
        return root
    vdso.extract_root=sparse
    try: metadata=fltk.prepare_disk(image,kernel,output,payload)
    finally: vdso.extract_root=original
    Path(metadata['root_image_snapshot']).unlink(); metadata['root_image_snapshot_removed']=True
    metadata['calculator_sha256']=fltk.sha256(elf)
    (output/'prepare.json').write_text(json.dumps(metadata,indent=2)+'\n')
    return metadata

class Session(CommonSession):
    def __init__(self,output):
        super().__init__(output)
        def key(value):
            codes=value.split('-')
            events=[{'type':'key','data':{'down':down,'key':{'type':'qcode','data':item}}}
                    for down,sequence in ((True,codes),(False,reversed(codes))) for item in sequence]
            self.probe.command('input-send-event',{'events':events}); time.sleep(.12)
        punctuation={'(':'shift-9',')':'shift-0','+':'shift-equal','*':'shift-8',
                     '^':'shift-6','%':'shift-5','!':'shift-1',',':'comma','<':'shift-comma',
                     '>':'shift-dot','&':'shift-7','|':'shift-backslash','~':'shift-grave_accent'}
        def text(value):
            for ch in value: key(punctuation.get(ch,text_keys(ch)[0]))
            time.sleep(.4)
        self.probe.key,self.probe.text=key,text
    def wait(self,text,offset=0,timeout=60):
        return xorg.wait_serial_after(self.serial,text,self.process,offset,timeout=timeout)
    def launch(self):
        self.launch_offset=len(self.serial.read_text(errors='replace'))
        self.probe.click(300,200); self.probe.key('ctrl-u'); self.probe.text('calctest'); self.probe.key('ret')
        deadline=time.monotonic()+60
        while time.monotonic()<deadline:
            current=self.serial.read_text(errors='replace')[self.launch_offset:]
            if 'CALC_CORE_FAIL' in current: raise AssertionError('Target calculator core failed: '+current[-1200:])
            if '[calc.elf] FLTK calculator ready' in current: break
            if self.process.poll() is not None: raise RuntimeError('QEMU exited')
            time.sleep(.2)
        else: raise AssertionError('Calculator did not launch')
        log=self.serial.read_text(errors='replace')[self.launch_offset:]
        assert 'CALC_CORE_PASS' in log
        self.pid=int(re.search(r'CALC_START pid=(\d+)',log)[1])
        self.geometry=fltk.wait_client_geometry(self.serial,self.process,self.pid,self.launch_offset)
        self.active_mode=0
        time.sleep(2)
    def enter(self,x,y,text,mode,statistics=False):
        self.click(x,y); self.fill(text)
        offset=len(self.serial.read_text(errors='replace'))
        self.probe.key('ctrl-ret' if statistics else 'ret')
        self.wait('[calc.elf] evaluated mode='+str(mode),offset)
    def choose(self,x,y,target,current):
        if target==current: return
        fields={240:(166,174),410:(352,140),370:(278,200),400:(166,574),300:(166,281),580:(459,281)}
        left,width=fields[x]; gx,gy=self.geometry[:2]; self.click(x,y)
        deadline=time.monotonic()+12
        while time.monotonic()<deadline:
            frame=self.frame('choice-popup'); column=gx+left+width-8
            runs=[]; start=None
            for row in range(frame.height):
                r,g,b=frame.getpixel((column,row))[:3]; selected=r<40 and g<40 and b>85
                if selected and start is None: start=row
                elif not selected and start is not None:
                    if 9<=row-start<=18: runs.append((start,row))
                    start=None
            if runs:
                top,bottom=min(runs,key=lambda run:abs((run[0]+run[1])/2-(gy+y))); pitch=bottom-top+4
                self.probe.click(gx+left+20,(top+bottom-1)//2+(target-current)*pitch)
                time.sleep(.5); return
        raise AssertionError('Native choice popup did not paint its selected row')
    def check(self,rect,expected,tag,timeout=12):
        # Read the actual FLTK output through normal selection/copy. SimSun
        # digits are unreliable OCR input; the reference consumer is a separate
        # process using only the public X11 clipboard protocol.
        a,b,w,h=rect
        self.click(a+w//2,b+h//2)
        self.probe.key('home'); self.probe.key('shift-end')
        offset=len(self.serial.read_text(errors='replace')); self.probe.key('ctrl-c')
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            records=re.findall(r'CALC_CLIPBOARD_HEX ([0-9a-f]+)',self.serial.read_text(errors='replace')[offset:])
            for record in records:
                text=bytes.fromhex(record).decode('utf-8')
                if text==expected:
                    (self.output/(tag+'.clipboard.txt')).write_text(text)
                    self.probe.key('end'); self.probe.key('ctrl-'+str(self.active_mode+1))
                    frame=self.frame(tag); x,y=self.geometry[:2]
                    frame.crop((x+a,y+b,x+a+w,y+b+h)).save(self.output/(tag+'-value.png'))
                    return text
            time.sleep(.15)
        raise AssertionError(f'{tag}: clipboard did not show {expected!r}; records={records[-3:]}')
    def mode(self,n):
        offset=len(self.serial.read_text(errors='replace')); self.probe.key('ctrl-'+str(n+1))
        self.wait('[calc.elf] mode='+str(n),offset)
        self.active_mode=n
    def close_app(self):
        offset=len(self.serial.read_text(errors='replace'))
        self.probe.click(self.geometry[0]+self.geometry[2]-10,self.geometry[1]-11)
        self.wait('CALC_EXIT status=0',offset)

def exercise(s):
    s.launch(); screenshots=[]
    def shot(name): s.screenshot(name); screenshots.append(name+'.png')
    s.enter(360,92,'(12.5+7.5)*3-5',0)
    s.check((168,124,568,39),'55','standard-result'); shot('calc-standard')
    s.click(440,193) # MS
    s.check((14,470,136,26),'55','memory-store')
    s.enter(360,92,'7',0); s.probe.text('+2'); s.probe.key('ret')
    s.check((168,124,568,39),'9','continue-result')
    s.enter(360,92,'2+3*4',0); s.probe.text('*2'); s.probe.key('ret')
    s.check((168,124,568,39),'28','continue-precedence')
    s.click(360,92); s.probe.text('9'); s.probe.key('equal')
    s.check((168,124,568,39),'9','fresh-number-and-equals')
    s.probe.key('esc'); s.click(310,193); s.probe.key('ret') # MR
    s.check((168,124,568,39),'55','memory-recall')
    s.mode(1); s.enter(360,92,'sqrt(81)+sin(30)*10+5!',1)
    s.check((168,124,568,39),'134','scientific-result'); shot('calc-scientific')
    s.choose(370,228,1,0); s.enter(360,92,'sin(pi/2)',1)
    s.check((168,124,568,39),'1','radian-result')
    s.mode(2); s.choose(240,65,1,0); s.probe.key('esc'); s.click(400,317); s.click(708,110)
    s.check((218,175,518,25),'12','hex-c-key')
    s.click(690,425); s.probe.key('ret')
    s.check((218,175,518,25),'18446744073709551603','bit-not-key')
    s.choose(240,65,0,1); s.enter(360,110,'18446744073709551615',2)
    s.check((218,175,518,25),'18446744073709551615','uint64-result'); shot('calc-programmer')
    s.probe.key('ctrl-shift-c'); s.probe.key('esc'); s.click(350,110); s.probe.key('ctrl-v'); s.probe.key('ret')
    s.check((218,175,518,25),'18446744073709551615','uint64-clipboard')
    s.choose(240,65,1,0); s.click(720,602)
    s.check((218,175,518,25),'18446744073709551614','bit-editor')
    s.choose(410,65,0,3); s.click(584,65)
    s.check((218,175,518,25),'-2','signed-byte')
    screenshots.extend(exercise_tools(s))
    return screenshots

def exercise_tools(s):
    screenshots=[]
    def shot(name): s.screenshot(name); screenshots.append(name+'.png')
    s.mode(3); s.enter(360,168,'1500',3)
    s.check((168,320,568,42),'1.5','length-result'); shot('calc-units-length')
    s.choose(400,88,4,0); s.enter(360,168,'100',3)
    s.check((168,320,568,42),'212','temperature-result'); shot('calc-units-temperature')
    s.choose(400,88,14,4); s.choose(300,252,8,0); s.choose(580,252,3,1)
    s.enter(360,168,'1',3); s.check((168,320,568,42),'1.048576','storage-result'); shot('calc-units-data')
    s.choose(400,88,6,14); s.enter(360,168,'10',3)
    s.check((168,320,568,42),'36','speed-result')
    s.mode(4); s.enter(350,130,'1,2,3,4,5',4,statistics=True)
    s.check((168,414,276,24),'3','mean-result')
    s.click(450,295); s.check((168,414,276,24),'25.5','example-mean'); shot('calc-statistics')
    s.mode(5); s.click(280,92); s.fill('2024-02-28'); s.enter(570,92,'2024-03-01',5)
    s.check((168,258,568,31),'2','date-difference')
    s.click(310,498); s.fill('30'); s.click(650,498)
    s.check((168,544,568,35),'2024-03-29','date-add')
    s.click(400,190); shot('calc-date')
    # Revisit every page after other calculations to prove independent inputs.
    s.mode(0); s.check((168,124,568,39),'55','preserved-standard')
    s.probe.key('esc'); s.click(360,92); s.probe.text('1/0'); s.probe.key('ret'); shot('calc-error')
    status=s.frame('error-status'); x,y=s.geometry[:2]
    pixels=status.crop((x+12,y+650,x+968,y+672)).convert('RGB')
    pixel_values=getattr(pixels,'get_flattened_data',pixels.getdata)()
    assert sum(r>150 and g<80 and b<80 for r,g,b in pixel_values)>30,'Error status was not visible'
    s.probe.key('esc'); s.enter(360,92,'3+4',0); s.check((168,124,568,39),'7','error-recovered')
    s.probe.key('f1'); screenshots.extend(finish_gui(s))
    return screenshots

def finish_gui(s):
    wait_focus(s,False)
    deadline=time.monotonic()+20
    origin=None
    while time.monotonic()<deadline and origin is None:
        frame=s.frame('help-paint')
        for y in range(frame.height-20):
            start=None
            for x in range(frame.width):
                r,g,b=frame.getpixel((x,y))[:3]; active=b>100 and b>r*1.5 and b>g*1.5
                if active and start is None: start=x
                elif not active and start is not None:
                    # Reject the wider main-window title while IceWM is still
                    # drawing the newly mapped modal. Require its white view.
                    if 660<=x-start<=760:
                        bottom=y
                        while bottom+1<frame.height and frame.getpixel((start+2,bottom+1))[:3]==frame.getpixel((start+2,y))[:3]: bottom+=1
                        hx,hy=start,bottom+2
                        if hy+20<frame.height and all(v>240 for v in frame.getpixel((hx+20,hy+20))[:3]):
                            origin=(hx,hy); break
                    start=None
            if origin: break
    if not origin: raise AssertionError('The help modal did not finish painting')
    hx,hy=origin; s.screenshot('calc-help')
    s.probe.click(hx+694,hy+533); wait_focus(s,True)
    verify_layout(s)
    s.close_app(); return ['calc-help.png','calc-minimum-size.png']

def verify_layout(s):
    resize(s,940,660)
    # ConfigureNotify can precede the FLTK back-buffer repaint in this debug
    # guest. Refocus using a native click after IceWM releases its resize grab,
    # exercise the resized page, then capture the settled client pixels.
    time.sleep(.7); s.click(60,86); s.active_mode=0
    s.check((168,124,528,39),'7','resized-result')
    time.sleep(.7); s.screenshot('calc-minimum-size')

def exercise_layout(s):
    s.launch(); s.enter(360,92,'3+4',0); s.check((168,124,568,39),'7','layout-initial-result')
    verify_layout(s); s.close_app(); return ['calc-minimum-size.png']

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image',type=Path,required=True); parser.add_argument('--kernel',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True); parser.add_argument('--interactive',action='store_true')
    parser.add_argument('--scenario',choices=('full','layout'),default='full')
    args=parser.parse_args(); output=args.output.resolve()
    if not output.is_relative_to(ROOT/'out') or output==ROOT/'out': parser.error('output must be a fresh out subdirectory')
    metadata=prepare(args.image.resolve(),args.kernel.resolve(),output); session=None
    state={'passed':False,'harness_resumed':False}
    try:
        session=Session(output); session.login(); print('Calculator guest desktop ready',flush=True)
        state['screenshots']=(exercise_layout if args.scenario=='layout' else exercise)(session); state['passed']=True
    except Exception:
        traceback.print_exc()
        if session: session.screenshot('verification-failure')
        if args.interactive:
            state['harness_resumed']=True
            code.interact(local={'s':session,'state':state,'exercise':exercise,'fltk':fltk,'time':time})
        if not state['passed']: raise
    finally:
        if session: session.close()
    assert fltk.sha256(args.image)==metadata['source_image_sha256_before']
    result={**state,'platform':'QEMU KVM / ReliefOS Xorg / IceWM',
            'source_image_unchanged':True,'vmware_verified':False,'default_vmdk_boot_verified':False,
            'calculator_sha256':metadata['calculator_sha256'],'kernel_sha256':metadata['kernel_hashes']['source_sha256']}
    (output/'result.json').write_text(json.dumps(result,indent=2)+'\n'); print(json.dumps(result,indent=2),flush=True)

if __name__=='__main__': main()
