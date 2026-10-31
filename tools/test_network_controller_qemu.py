#!/usr/bin/env python3
"""Exercise the shipped Network Controller on a private image and network.

Run with unshare -Urn. GUI inspection uses the recorded QMP socket. Creating
reboot-request runs guest poweroff and performs a second boot; creating finish
ends only this script's VM. No production app/kernel is replaced in the image.
"""
from pathlib import Path
import argparse
import fcntl
import hashlib
import json
import os
import re
import socket
import struct
import subprocess
import threading
import time
import test_vdso_qemu as disk
from test_installer_accounts_qemu import Probe
from test_openrc_controller_qemu import login
from run_gcc_probe_qemu import qmp_quit

ROOT=Path(__file__).resolve().parents[1]
APP='/usr/lib/reliefos/apps/network-controller/network-controller.elf'
QA='/usr/lib/reliefos/tests/network-controller'

def run(command,**kw):
    return subprocess.run(list(map(str,command)),cwd=ROOT,check=True,**kw)
def digest(path):
    with Path(path).open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
def prepare(build,out,port):
    out.mkdir(parents=True,exist_ok=False)
    image=build/'images/reliefos.vmdk';evidence={'image':str(image),'image_sha256':digest(image),'vmware_verified':False,'checks':[]}
    raw,layout=disk.private_raw_image(image,out);root=disk.extract_root(raw,layout,out);commands=[]
    def read(path,target):run(['debugfs','-R',f'dump {path} "{target}"',root],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    def put(source,target,mode='0100644',uid=0,gid=0):
        disk.add_missing_root_directory(root,str(Path(target).parent),commands)
        if disk.debugfs_exists(root,target):commands.append('rm '+target)
        commands.extend([f'write "{source}" {target}',f'set_inode_field {target} mode {mode}',f'set_inode_field {target} uid {uid}',f'set_inode_field {target} gid {gid}'])
    def text(name,value,target,mode='0100644'):
        path=out/name;path.write_text(value);put(path,target,mode)
    shipped=out/'shipped-app.elf';read(APP,shipped)
    assert digest(shipped)==digest(build/'userland/network-controller.elf')
    evidence['app_sha256']=digest(shipped)
    shipped_kernel=out/'shipped-kernel.sys'
    run(['mcopy','-i',f'{raw}@@{layout["esp_first"]*512}','::/reliefos/kernel.sys',shipped_kernel])
    assert digest(shipped_kernel)==digest(build/'generated/system/kernel.sys')
    evidence['kernel_sha256']=digest(shipped_kernel)
    compiler=build/'sdk/reliefos-musl-sdk/bin/reliefos-musl-cc'
    run([compiler,'-O2','-Wall','-Wextra','-x','c++','-std=c++17','-nostdinc++','-fno-exceptions','-fno-rtti','-I'+str(ROOT/'userland/apps/network-controller'),ROOT/'tools/tests/network_controller_guest.cpp',ROOT/'userland/apps/network-controller/model.cpp','-o',out/'guest-probe'])
    run([compiler,'-O2','-Wall','-Wextra','-isystem',build/'upstream/x11-development/usr/include',ROOT/'tools/tests/openrc_controller_window_probe.c','-L'+str(build/'upstream/x11-development/usr/lib'),'-lX11','-o',out/'window-probe'])
    put(out/'guest-probe',QA+'/guest-probe','0100755');put(out/'window-probe',QA+'/window-probe','0100755')
    for database in ('passwd','shadow','group','gshadow'):
        path=out/('private-'+database);read('/etc/'+database,path)
        lines=[s for s in path.read_text().splitlines() if not s.startswith('test:')]
        lines.extend(s for s in (ROOT/'system/test-accounts'/database).read_text().splitlines() if s.startswith('test:'))
        if database=='shadow':
            hashed=run(['openssl','passwd','-6','-stdin'],input='network-root\n',text=True,capture_output=True).stdout.strip()
            lines=[':'.join(['root',hashed,*s.split(':')[2:]]) if s.startswith('root:') else s for s in lines]
        if database=='group':
            lines=[':'.join([*s.split(':')[:3],','.join(sorted(set(filter(None,s.split(':')[3].split(',')))|{'test'}))]) if s.startswith('wheel:') else s for s in lines]
        path.write_text('\n'.join(lines)+'\n');path.chmod(0o600)
        put(path,'/etc/'+database,'0100600' if 'shadow' in database else '0100644')
    disk.add_missing_root_directory(root,'/home/test',commands)
    commands.extend(['set_inode_field /home/test uid 1000','set_inode_field /home/test gid 1000','set_inode_field /home/test mode 040700'])
    skel=build/'stage/disk-root/etc/skel'
    for path in sorted(skel.rglob('*')):
        target='/home/test/'+str(path.relative_to(skel))
        if path.is_file():put(path,target,'0100755' if os.access(path,os.X_OK) else '0100644',1000,1000)
    text('guest-runner',f'#!/bin/sh\n{QA}/guest-probe {port}\nstatus=$?\necho "NC_GUEST_DONE status=$status" >/dev/ttyS0\nexit "$status"\n',QA+'/runner','0100755')
    text('qa-service',f'''#!/sbin/openrc-run
description="Private Network Controller acceptance"
depend() {{ after reliefos-dhcp; }}
start() {{ /bin/sh {QA}/runner </dev/null >/dev/ttyS0 2>&1 & return 0; }}
''','/etc/init.d/network-controller-qa','0100755')
    commands.append('symlink /etc/runlevels/default/network-controller-qa /etc/init.d/network-controller-qa')
    text('gui-runner',f'''#!/bin/sh
export LANG=zh_CN.UTF-8 LC_ALL=zh_CN.UTF-8
if [ -n "${{XAUTHORITY:-}}" ] && [ "$XAUTHORITY" != "$HOME/.Xauthority" ]; then cp "$XAUTHORITY" "$HOME/.Xauthority";chmod 600 "$HOME/.Xauthority";fi
{QA}/window-probe >/dev/ttyS0 2>&1 &
observer=$!
{APP} >/dev/ttyS0 2>&1 &
pid=$!
echo "NC_GUI_START pid=$pid" >/dev/ttyS0
wait "$pid"
echo "NC_GUI_EXIT status=$?" >/dev/ttyS0
kill "$observer" 2>/dev/null
''','/usr/bin/ncqa','0100755')
    disk.add_missing_root_directory(root,'/var/lib/reliefos',commands)
    script=out/'inject.commands';script.write_text('\n'.join(dict.fromkeys(commands))+'\n')
    with (out/'inject.log').open('w') as log:run(['debugfs','-w','-f',script,root],stdout=log,stderr=subprocess.STDOUT)
    disk.put_root_back(raw,root,layout);evidence['layout']=layout
    (out/'prepare.json').write_text(json.dumps(evidence,ensure_ascii=False,indent=2));return evidence

def dns_fixture(server,stop,counters):
    server.settimeout(.3)
    while not stop.is_set():
        try:packet,peer=server.recvfrom(4096)
        except socket.timeout:continue
        if len(packet)<12:continue
        pos=12
        while pos<len(packet) and packet[pos]:pos+=packet[pos]+1
        end=pos+5
        if end>len(packet):continue
        kind=int.from_bytes(packet[pos+1:pos+3],'big');answer=kind==1
        response=packet[:2]+struct.pack('!5H',0x8180,1,int(answer),0,0)+packet[12:end]
        if answer:response+=b'\xc0\x0c'+struct.pack('!HHIH',1,1,60,4)+socket.inet_aton('10.0.2.2')
        server.sendto(response,peer);counters['dns']+=1

def tcp_fixture(server,stop,counters):
    server.settimeout(.3)
    while not stop.is_set():
        try:client,unused=server.accept()
        except socket.timeout:continue
        client.close();counters['tcp']+=1

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--build-dir',type=Path,default=ROOT/'out/xorg-vmware-debug')
    args=parser.parse_args();out=args.output.resolve()
    if not out.is_relative_to(ROOT/'out') or out.exists():parser.error('output must be a new directory below out/')
    namespace=os.open('/proc/self/ns/net',os.O_RDONLY)
    try:
        owner=fcntl.ioctl(namespace,0xb701)
        try:assert os.geteuid()==0 and os.fstat(owner).st_ino==os.stat('/proc/self/ns/user').st_ino,'Run with unshare -Urn'
        finally:os.close(owner)
    finally:os.close(namespace)
    run(['ip','link','set','lo','up'])
    dns=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);dns.bind(('0.0.0.0',53))
    tcp=socket.socket();tcp.bind(('127.0.0.1',0));tcp.listen();port=tcp.getsockname()[1]
    stop=threading.Event();counters={'dns':0,'tcp':0}
    for function,server in ((dns_fixture,dns),(tcp_fixture,tcp)):
        threading.Thread(target=function,args=(server,stop,counters),daemon=True).start()
    evidence=prepare(args.build_dir.resolve(),out,port);process=None;qmp=None;probe=None;log=None
    def boot(index):
        qmp=ROOT.parent/f'.reliefos-network-{os.getpid()}-{index}.sock'
        serial=out/('serial.log' if index==0 else 'serial-reboot.log')
        command=['qemu-system-x86_64','-enable-kvm','-cpu','host','-machine','q35','-m','4096','-smp','1','-bios','/usr/share/edk2/x64/OVMF.4m.fd','-display','none','-serial',f'file:{serial}','-vga','vmware','-device','qemu-xhci','-device','usb-tablet','-netdev','user,id=net0','-device','e1000,netdev=net0','-drive',f'file={out/"guest.raw"},format=raw,if=ide','-qmp',f'unix:{qmp},server=on,wait=off','-no-reboot','-no-shutdown']
        log=(out/f'qemu-{index}.log').open('w');p=subprocess.Popen(command,stdout=log,stderr=log,cwd=ROOT,start_new_session=True)
        (out/'qemu.json').write_text(json.dumps({'pid':p.pid,'qmp':str(qmp),'serial':str(serial),'command':command},indent=2))
        deadline=time.monotonic()+15
        while not qmp.exists():
            if p.poll() is not None or time.monotonic()>deadline:raise RuntimeError('QEMU failed to start')
            time.sleep(.1)
        return p,qmp,serial,log
    def wait_native(serial,process):
        deadline=time.monotonic()+360
        while time.monotonic()<deadline:
            text=serial.read_text(errors='replace') if serial.exists() else ''
            if 'NC_GUEST_DONE' in text:
                assert 'NC_GUEST_DONE status=0' in text,text[-5000:]
                return text
            if process.poll() is not None:raise RuntimeError('QEMU exited before native checks')
            time.sleep(.5)
        raise TimeoutError('Native acceptance timed out')
    try:
        process,qmp,serial,log=boot(0);text=wait_native(serial,process)
        evidence['checks'].append('shipped worker: DHCP/static/disabled, backup restore, DNS, Ping and TCP fixtures')
        evidence['native_checks']=int(re.search(r'NETWORK-CONTROLLER PASS checks=(\d+)',text)[1])
        print('Native Network Controller checks passed',flush=True)
        probe=Probe(qmp,out);login(probe,process)
        probe.click(300,200);probe.text('ncqa');probe.key('ret')
        deadline=time.monotonic()+60
        while 'title=网络身份认证' not in serial.read_text(errors='replace'):
            if time.monotonic()>deadline:raise TimeoutError('Chinese authentication window did not appear')
            time.sleep(.25)
        probe.frame('01-authentication');probe.stream.close();probe.socket.close();probe=None
        print('Chinese authentication visible; QMP and PID recorded in qemu.json',flush=True)
        (out/'verification.json').write_text(json.dumps(evidence,ensure_ascii=False,indent=2))
        while process.poll() is None and not (out/'finish').exists():
            if (out/'reboot-request').exists() and not (out/'reboot-complete').exists():
                control=Probe(qmp,out)
                # Close the tested app before requesting this transition. The
                # stock terminal then owns input; authenticate the private test
                # account and let OpenRC perform a normal guest shutdown.
                control.click(300,200)
                control.text('sudo -k /sbin/poweroff');control.key('ret')
                time.sleep(2)
                control.text('test');control.key('ret')
                deadline=time.monotonic()+120
                while time.monotonic()<deadline:
                    if control.command('query-status').get('status')=='shutdown':break
                    time.sleep(.5)
                else:raise TimeoutError('Guest poweroff did not complete')
                control.stream.close();control.socket.close();qmp_quit(qmp,process);log.close()
                process,qmp,serial,log=boot(1);text=wait_native(serial,process)
                assert 'NC_BOOT_PERSISTENCE_PASS' in text
                evidence['checks'].append('static address and DNS persisted across guest poweroff and reboot')
                (out/'reboot-complete').write_text('passed\n')
                (out/'verification.json').write_text(json.dumps(evidence,ensure_ascii=False,indent=2))
                print('Native reboot persistence passed',flush=True)
            time.sleep(.5)
        evidence['fixture_requests']=counters
        evidence['source_image_unchanged']=digest(args.build_dir.resolve()/'images/reliefos.vmdk')==evidence['image_sha256']
        (out/'verification.json').write_text(json.dumps(evidence,ensure_ascii=False,indent=2))
    finally:
        if probe:probe.stream.close();probe.socket.close()
        if process:qmp_quit(qmp,process)
        if log:log.close()
        stop.set();dns.close();tcp.close()
if __name__=='__main__':main()
