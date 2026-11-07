#!/usr/bin/env python3
"""Verify ReliefAPK with real signed packages on a private ReliefOS QEMU disk."""
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
from apk_distribution import make_package
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'out/xorg-vmware-debug'
FIX='/usr/lib/reliefos/tests/reliefapk'
ENV={**os.environ,'TMPDIR':str(ROOT/'out/tmp')}


def run(args,**kw):
    return subprocess.run(list(map(str,args)),cwd=ROOT,env=ENV,check=True,**kw)


def prepare(image,kernel,output):
    output.mkdir(parents=True,exist_ok=False)
    (ROOT/'out/tmp').mkdir(exist_ok=True)
    fixtures=output/'fixtures';fixtures.mkdir()
    key=fixtures/'test-key.pem'
    run(['openssl','genpkey','-algorithm','RSA','-pkeyopt','rsa_keygen_bits:2048','-out',key],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    key.chmod(0o600)
    keys=fixtures/'keys';keys.mkdir()
    public=keys/'reliefapk-test.rsa.pub'
    run(['openssl','pkey','-in',key,'-pubout','-out',public],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    apk=OUT/'upstream/apk/apk.static'
    packages=[]
    for version,folder in [('1.0-r0','v1'),('2.0-r0','v2')]:
        repo=fixtures/folder;repo.mkdir()
        for name,deps in [('reliefapk-dependency',[]),('reliefapk-demo',['reliefapk-dependency'])]:
            payload=fixtures/(folder+'-'+name);target=payload/'usr/share/reliefapk-demo';target.mkdir(parents=True)
            (target/(name+'.txt')).write_text(version+'\n')
            package=repo/(name+'-'+version+'.apk')
            make_package(apk,key,payload,package,name,version,deps)
            packages.append(package)
        run([apk,'mkndx','--keys-dir',keys,'--sign-key',key,'--output',repo/'packages.adb',*sorted(repo.glob('*.apk'))],stdout=subprocess.DEVNULL)
    probe=fltk.target_compile_probe(output,OUT/'sysroot/musl','clang++','ld.lld')
    commands=json.loads((output/'probe-build.command.json').read_text())
    commands['compile'][-3]=str(ROOT/'tools/tests/reliefapk_window_probe.cpp');commands['compile'][-1]=str(output/'ra-window.o')
    commands['link'][commands['link'].index(str(output/'fltk_ui_probe.o'))]=str(output/'ra-window.o')
    commands['link'][commands['link'].index(str(probe))]=str(output/'ra-window.elf')
    with (output/'window-probe-build.log').open('w') as log:
        for cmd in commands.values():run(cmd,stdout=log,stderr=log)
    bench_compile=commands['compile'].copy();bench_compile.insert(1,'-I'+str(ROOT/'userland/apps/reliefapk'));bench_link=commands['link'].copy()
    bench_compile[-3]=str(ROOT/'tools/tests/reliefapk_catalog_benchmark.cpp');bench_compile[-1]=str(output/'ra-benchmark.o')
    bench_link[bench_link.index(str(output/'ra-window.o'))]=str(output/'ra-benchmark.o')
    bench_link[bench_link.index(str(output/'ra-window.elf'))]=str(output/'ra-benchmark.elf')
    bench_link.insert(bench_link.index('-L'+str(OUT/'sysroot/musl/lib')) if '-L'+str(OUT/'sysroot/musl/lib') in bench_link else bench_link.index('-L'),str(OUT/'obj/app-reliefapk/userland/apps/reliefapk/model.cpp.o'))
    with (output/'benchmark-build.log').open('w') as log:
        run(bench_compile,stdout=log,stderr=log);run(bench_link,stdout=log,stderr=log)
    runner=fixtures/'ra';runner.write_text('''#!/bin/sh
export LC_ALL=zh_CN.UTF-8 LANG=zh_CN.UTF-8
/usr/lib/reliefos/tests/reliefapk-catalog-benchmark 20000 >/dev/ttyS0 2>&1
/usr/lib/reliefos/apps/reliefapk/reliefapk.elf "$@" >/dev/ttyS0 2>&1 &
pid=$!
echo "RA_START pid=$pid" >/dev/ttyS0
/usr/lib/reliefos/tests/reliefapk-window-probe "$pid" >/dev/ttyS0 2>&1 &
watcher=$!
wait "$pid"
status=$?
kill "$watcher" 2>/dev/null
echo "RA_EXIT status=$status" >/dev/ttyS0
''')
    entry=fixtures/'entry';entry.write_text('#!/bin/sh\nexec /usr/lib/reliefos/apps/reliefapk/reliefapk.elf "$@"\n')
    repositories=fixtures/'repositories';repositories.write_text('# ReliefAPK private acceptance repositories\n/usr/share/reliefos/apk/repository/packages.adb\n'+FIX+'/v1/packages.adb\n')
    preferences=fixtures/'reliefapk.conf';preferences.write_text('offline=1\ntimeout=15\ncache_max_age=10080\nauth=sudo\n')
    payload=[(OUT/'userland/reliefapk.elf','/usr/lib/reliefos/apps/reliefapk/reliefapk.elf',0o100755),
        (OUT/'generated/nls/zh_CN/LC_MESSAGES/leonos.mo',fltk.LOCALE_GUEST,0o100644),
        (ROOT/'resources/build-art/app-icons/reliefapk.bmp','/usr/lib/reliefos/apps/reliefapk/reliefapk.bmp',0o100644),
        (ROOT/'userland/apps/reliefapk/reliefapk.desktop','/usr/share/applications/reliefos-reliefapk.desktop',0o100644),
        (entry,'/usr/bin/reliefapk',0o100755),(runner,'/usr/bin/ra',0o100755),
        (output/'ra-window.elf','/usr/lib/reliefos/tests/reliefapk-window-probe',0o100755),
        (output/'ra-benchmark.elf','/usr/lib/reliefos/tests/reliefapk-catalog-benchmark',0o100755),
        (public,'/etc/apk/keys/'+public.name,0o100644),
        (repositories,'/etc/apk/repositories',0o100644),
        (preferences,'/home/test/.config/reliefapk.conf',0o100600)]
    for package in packages:payload.append((package,FIX+'/'+package.parent.name+'/'+package.name,0o100644))
    for folder in ('v1','v2'):payload.append((fixtures/folder/'packages.adb',FIX+'/'+folder+'/packages.adb',0o100644))
    fltk.validate_target_elf(OUT/'userland/reliefapk.elf',sysroot=OUT/'sysroot/musl',required_needed=('libfltk.so.1.3',))
    metadata=fltk.prepare_disk(image,kernel,output,payload)
    part=next(p for p in metadata['gpt_partitions'] if p['name']=='RELIEFOS_ROOT')
    layout={'root_first':part['first'],'root_last':part['last']}
    root=Path(metadata['root_image_snapshot'])
    # Only private credentials are set; the source disk and host accounts are untouched.
    shadow=run(['debugfs','-R','cat /etc/shadow',root],capture_output=True,text=True).stdout
    hashed=run(['openssl','passwd','-6','-stdin'],input='reliefapk-root\n',capture_output=True,text=True).stdout.strip()
    lines=shadow.splitlines();assert any(line.startswith('test:') for line in lines),'base image must contain test/test account'
    lines=[':'.join([line.split(':')[0],hashed,*line.split(':')[2:]]) if line.startswith('root:') else line for line in lines]
    shadow_file=fixtures/'private-shadow';shadow_file.write_text('\n'.join(lines)+'\n');shadow_file.chmod(0o600)
    changes=output/'private-account.commands';changes.write_text('\n'.join([
        'rm /etc/shadow',f'write {fltk.debugfs_quote(shadow_file)} /etc/shadow','set_inode_field /etc/shadow mode 0100600',
        'set_inode_field /home/test/.config uid 1000','set_inode_field /home/test/.config gid 1000',
        'set_inode_field /home/test/.config/reliefapk.conf uid 1000','set_inode_field /home/test/.config/reliefapk.conf gid 1000'])+'\n')
    run(['debugfs','-w','-f',changes,root],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    vdso.put_root_back(output/'guest-private.raw',root,layout)
    metadata['private_image_sha256']=fltk.sha256(output/'guest-private.raw')
    (output/'prepare.json').write_text(json.dumps(metadata,ensure_ascii=False,indent=2)+'\n')
    return metadata


class Session(BaseSession):
    def login(self):
        # A parallel image build can delay Xorg beyond the generic 45 s probe.
        # Wait for the actual login panel before sending any credentials.
        xorg.wait_greeter_stage(self.probe,self.process,'login','tty1-greeter',timeout=120)
        super().login()
    def wait(self,pattern,timeout=40,offset=0):
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            s=self.serial.read_text(errors='replace')[offset:]
            match=re.search(pattern,s)
            if match:return match
            if self.process.poll() is not None:raise RuntimeError('QEMU exited')
            time.sleep(.15)
        self.probe.frame('timeout')
        raise AssertionError('Missing serial marker: '+pattern)
    def launch(self,argument=''):
        offset=len(self.serial.read_text(errors='replace'))
        self.launch_offset=offset
        self.probe.click(180,330);self.probe.text('ra'+(' '+argument if argument else ''));self.probe.key('ret')
        self.pid=int(self.wait(r'RA_START pid=(\d+)',offset=offset)[1]);self.wait(r'RELIEFAPK_QUERY filter=1 rows=\d+',offset=offset)
        self.wait('RA_ACTIVE ',offset=offset);self.geometry=self.active();return self.geometry
    def active(self):
        time.sleep(.25)
        found=re.findall(r'RA_ACTIVE id=\d+ x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+) title=([^\n]*)',self.serial.read_text(errors='replace'))
        if not found:raise AssertionError('No active ReliefAPK window')
        return tuple(map(int,found[-1][:4]))
    def click_active(self,x,y):
        g=self.active();self.probe.click(g[0]+x,g[1]+y)
    def main_click(self,x,y):self.probe.click(self.geometry[0]+x,self.geometry[1]+y)
    def password(self,value):
        deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            states=re.findall(r'RA_AUTH_WINDOW visible=(\d)(?: x=(-?\d+) y=(-?\d+) w=500 h=226)?',self.serial.read_text(errors='replace')[self.launch_offset:])
            if states and states[-1][0]=='1':
                x,y=map(int,states[-1][1:]);break
            time.sleep(.1)
        else:raise AssertionError('Mapped authentication window not observed')
        self.probe.click(x+240,y+110);self.probe.text(value);self.probe.click(x+420,y+181)
    def wait_authed(self,pattern,offset,timeout=180):
        deadline=time.monotonic()+timeout;seen=offset
        while time.monotonic()<deadline:
            log=self.serial.read_text(errors='replace')
            match=re.search(pattern,log[offset:])
            if match:return match
            requests=list(re.finditer(r'RELIEFAPK_AUTH requested mode=(\d+)',log[seen:]))
            if requests:
                mode=int(requests[-1][1]);seen=len(log)
                self.password('reliefapk-root' if mode else 'test')
            time.sleep(.2)
        self.screenshot('timeout-auth-operation');raise AssertionError('Operation did not complete: '+pattern)
    def replace_active(self,x,y,text):
        self.click_active(x,y)
        for key in ('home','shift-end','backspace'):self.probe.key(key)
        self.probe.text(text)
    def menu(self,item):
        self.main_click(80,14);time.sleep(.3);self.main_click(170,40+22*item)
    def apply_queue(self,name):
        self.main_click(875,50);offset=len(self.serial.read_text(errors='replace'));self.click_active(475,375)
        self.wait(r'RELIEFAPK_OPERATION mode=1 exit=0',offset=offset);self.screenshot(name+'-preview')
        offset=len(self.serial.read_text(errors='replace'));self.click_active(615,513)
        self.wait_authed(r'RELIEFAPK_OPERATION mode=4 exit=0',offset);self.screenshot(name+'-completed');self.click_active(775,513)
        time.sleep(.6)
    def screenshot(self,name):
        time.sleep(1)
        return self.frame(name)
    def close(self):
        super().close()


def exercise(session):
    s=session
    # Exercise su before the longer package lifecycle so an authentication
    # regression fails promptly. The first sudo attempt remains uncached.
    s.menu(1);time.sleep(.5);s.click_active(400,175);s.probe.key('down');s.probe.key('ret');s.click_active(530,298)
    offset=len(s.serial.read_text(errors='replace'));s.main_click(175,50)
    s.wait_authed(r'RELIEFAPK_OPERATION mode=5 exit=0',offset);s.screenshot('00-root-authenticated');s.click_active(775,513)
    assert 'RELIEFAPK_AUTH requested mode=1' in s.serial.read_text(errors='replace')[offset:], 'root password was not requested'
    s.menu(1);time.sleep(.5);s.click_active(400,175);s.probe.key('up');s.probe.key('ret');s.click_active(530,298)
    s.screenshot('01-main-installed')
    # An offline local-index refresh still uses real sudo/PAM authentication.
    offset=len(s.serial.read_text(errors='replace'));s.main_click(175,50)
    s.wait('RELIEFAPK_AUTH requested mode=0',offset=offset);s.screenshot('02-sudo-authentication')
    retry=len(s.serial.read_text(errors='replace'));s.password('RA-SECRET-NEVER-LOG')
    s.wait('RELIEFAPK_AUTH requested mode=0',offset=retry,timeout=25)
    s.password('test');s.wait(r'RELIEFAPK_OPERATION mode=5 exit=0',offset=offset)
    s.screenshot('03-index-refresh');s.click_active(775,513)
    offset=len(s.serial.read_text(errors='replace'));s.main_click(55,138);s.wait(r'RELIEFAPK_QUERY filter=0 rows=\d+',offset=offset)
    s.main_click(370,95);s.probe.text('reliefapk-demo');time.sleep(.7);s.main_click(245,167);time.sleep(.6)
    s.screenshot('04-package-details');s.main_click(215,654);s.main_click(875,50);s.screenshot('05-pending-changes')
    offset=len(s.serial.read_text(errors='replace'));s.click_active(475,375);s.wait(r'RELIEFAPK_OPERATION mode=1 exit=0',offset=offset);s.screenshot('06-simulation-preview')
    offset=len(s.serial.read_text(errors='replace'));s.click_active(615,513);s.wait_authed(r'RELIEFAPK_OPERATION mode=4 exit=0',offset);s.screenshot('07-installed');s.click_active(775,513)
    # The repository editor preserves comments while switching the private fixture to v2.
    s.menu(0);time.sleep(.7);s.screenshot('08-repositories')
    s.click_active(300,86);s.replace_active(350,328,FIX+'/v2/packages.adb');s.click_active(180,417)
    offset=len(s.serial.read_text(errors='replace'));s.click_active(650,482)
    s.wait_authed(r'RELIEFAPK_OPERATION mode=6 exit=0',offset);s.screenshot('09-repositories-saved');s.click_active(775,513)
    s.click_active(778,482)
    offset=len(s.serial.read_text(errors='replace'));s.main_click(175,50)
    s.wait_authed(r'RELIEFAPK_OPERATION mode=5 exit=0',offset);s.click_active(775,513)
    offset=len(s.serial.read_text(errors='replace'));s.main_click(55,166)
    s.wait(r'RELIEFAPK_QUERY filter=2 rows=\d+',offset=offset);s.main_click(245,167)
    s.screenshot('10-upgradable');s.main_click(450,647);s.apply_queue('11-upgrade')
    # Root authentication uses the configured system su/PAM, without restarting the GUI.
    s.menu(1);time.sleep(.5);s.click_active(400,175);s.probe.key('down');s.probe.key('ret')
    s.screenshot('12-root-preferences');s.click_active(530,298)
    offset=len(s.serial.read_text(errors='replace'));s.main_click(55,150)
    s.wait(r'RELIEFAPK_QUERY filter=1 rows=\d+',offset=offset);s.main_click(245,167);s.main_click(581,647)
    s.apply_queue('12-root-repair')
    s.main_click(245,167);s.main_click(325,647);s.apply_queue('13-remove')
    s.main_click(370,95)
    for key in ('home','shift-end','backspace'):s.probe.key(key)
    time.sleep(.5);s.main_click(245,167);time.sleep(.8);s.screenshot('14-main-final')
    offset=len(s.serial.read_text(errors='replace'));s.probe.key('ctrl-q');s.wait(r'RA_EXIT status=0',offset=offset)
    # The standard desktop entry passes local APK paths to the same entry point.
    s.launch(FIX+'/v2/reliefapk-demo-2.0-r0.apk');s.apply_queue('15-local-apk')
    s.main_click(370,95);s.probe.text('reliefapk-demo');time.sleep(.8);s.main_click(245,167);time.sleep(.8)
    s.main_click(325,647);s.apply_queue('16-local-remove')
    offset=len(s.serial.read_text(errors='replace'));s.probe.key('ctrl-q');s.wait(r'RA_EXIT status=0',offset=offset)
    s.launch(FIX+'/v2/missing-package.apk');s.main_click(875,50)
    offset=len(s.serial.read_text(errors='replace'));s.click_active(475,375)
    s.wait(r'RELIEFAPK_OPERATION mode=1 exit=[1-9]\d*',offset=offset);s.screenshot('17-failed-simulation')
    assert not re.search(r'RELIEFAPK_OPERATION mode=4',s.serial.read_text(errors='replace')[offset:]), 'failed preview executed'
    s.click_active(775,513)
    offset=len(s.serial.read_text(errors='replace'));s.probe.key('ctrl-q');s.wait(r'RA_EXIT status=0',offset=offset)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image',type=Path,default=ROOT/'build/fltk-migration/baseline/xorg-test.raw')
    p.add_argument('--kernel',type=Path,default=OUT/'generated/system/kernel.sys')
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--prepare-only',action='store_true')
    p.add_argument('--resume',action='store_true')
    args=p.parse_args();output=args.output.resolve()
    metadata=json.loads((output/'prepare.json').read_text()) if args.resume else prepare(args.image.resolve(),args.kernel.resolve(),output)
    if args.prepare_only:return 0
    session=None
    try:
        session=Session(output);session.login();session.launch();exercise(session)
    finally:
        if session is not None:session.screenshot('last-frame');session.close()
    serial=(output/'serial.log').read_text(errors='replace')
    assert re.search(r'RELIEFAPK_CATALOG_BENCH .*verified=1',serial), 'catalog benchmark did not verify'
    assert 'RA_PAYLOAD value=1.0-r0' in serial and 'RA_PAYLOAD value=2.0-r0' in serial, 'installed/upgrade payloads not observed'
    assert 'RA-SECRET-NEVER-LOG' not in serial and 'reliefapk-root' not in serial, 'password leaked into logs'
    assert 'RELIEFAPK_AUTH requested mode=1' in serial, 'root-password authentication was not exercised'
    extracted=output/'verified';extracted.mkdir()
    part=next(p for p in metadata['gpt_partitions'] if p['name']=='RELIEFOS_ROOT')
    root=vdso.extract_root(output/'guest-private.raw',{'root_first':part['first'],'root_last':part['last']},extracted)
    repositories=run(['debugfs','-R','cat /etc/apk/repositories',root],capture_output=True,text=True).stdout
    assert '# ReliefAPK private acceptance repositories' in repositories and FIX+'/v2/packages.adb' in repositories
    installed=run(['debugfs','-R','cat /lib/apk/db/installed',root],capture_output=True,text=True).stdout
    assert 'P:reliefapk-demo\n' not in installed, 'package was not actually removed'
    assert fltk.sha256(args.image.resolve()) == metadata['source_image_sha256_before'], 'source image changed'
    result={'verified_repository_comment_preservation':True,'verified_removal':True,'source_image_unchanged':True,'vmware_verified':False}
    (output/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
    return 0
if __name__=='__main__':raise SystemExit(main())
