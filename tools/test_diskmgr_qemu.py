#!/usr/bin/env python3
"""Interactive Diskmgr X11 QA on a private copy of an installed fixture.

Reads JSON commands from stdin, records real QMP screenshots and fresh serial
job results, and creates a separate 512MiB scratch disk. It never opens host
block devices. The template must contain the standard test accounts root/r
and alice/USER_PASSWORD from test_installer_accounts_qemu.py. Commands:
  {"items":[{"click":[110,133]}],"job":0,"frame":"data"}
  {"op":"terminal","command":"id > /dev/ttyS0","expected":"uid="}
  {"op":"geometry"}, {"op":"table"}, {"op":"hash"}, {"op":"quit"}
Clicks use client coordinates; coords=dialog/password selects a centered modal.
Passwords use {"password":"user"} or {"password":"root"}; never log real secrets.
"""
from pathlib import Path
import argparse,hashlib,json,os,re,subprocess,sys,time,traceback
W=Path(__file__).resolve().parents[1]
os.chdir(W);sys.path.insert(0,str(W/'tools'))
from test_dual_boot_qemu import boot,wait_xdm,wait_for,greeter_crop,digest,copy_disk
from test_xorg_vm_qemu import _bright_panel_bbox,_session_st_visible
from test_installer_accounts_qemu import USER_PASSWORD
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--template',type=Path,required=True,help='Immutable installed QEMU fixture with root/r and alice/U!+30*u accounts')
parser.add_argument('--build',type=Path,default=Path('out/dualboot-build'))
parser.add_argument('--out',type=Path,required=True,help='New private directory; keep its QMP socket path short')
args=parser.parse_args()
out=args.out.resolve();out.mkdir(parents=True,exist_ok=False)
source=args.template.resolve();build=args.build.resolve()
assert source.is_file() and not source.is_block_device(), 'Use a regular fixture image, never a host device'
probe_dir=out/'probe';probe_dir.mkdir()
compiler=os.environ.get('TARGET_CC','clang')
resource=subprocess.check_output([compiler,'-print-resource-dir'],text=True).strip()
compile_base=[compiler,'--target=x86_64-linux-musl','-O2','-fPIE','-nostdinc','-isystem',str(build/'kernel-export/include'),'-isystem',str(build/'sysroot/musl/include'),'-isystem',resource+'/include','-isystem',str(build/'upstream/x11-development/usr/include')]
commands=[]
with (probe_dir/'build.log').open('w') as log:
 for name,filename,x11 in [('dm-x11-probe','diskmgr_x11_probe.c',True),('dm-block-probe','diskmgr_block_permissions.c',False)]:
  compile_cmd=compile_base+['-c',str(W/'tools/tests'/filename),'-o',str(probe_dir/(name+'.o'))]
  link=[os.environ.get('TARGET_LD','ld.lld'),'--gc-sections','-z','max-page-size=0x1000','-pie','--dynamic-linker','/lib/ld-musl-x86_64.so.1','-rpath','/usr/lib/reliefos:/lib:/usr/lib','-o',str(probe_dir/(name+'.elf')),str(build/'sysroot/musl/lib/Scrt1.o'),str(build/'sysroot/musl/lib/crti.o'),str(probe_dir/(name+'.o')),'-L',str(build/'sysroot/musl/lib')]+([str(build/'upstream/x11-development/usr/lib/libX11.so')] if x11 else [])+['-lc',str(build/'sysroot/musl/lib/crtn.o')]
  for command in (compile_cmd,link):
   commands.append(command);subprocess.run(command,check=True,stdout=log,stderr=subprocess.STDOUT)
(probe_dir/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
system=out/'system.raw';copy_disk(source,system)
table=json.loads(subprocess.check_output(['sfdisk','--json',system]))['partitiontable']
part=next(p for p in table['partitions'] if p['type'].lower()=='0fc63daf-8483-4772-8e79-3d69d8477de4')
root=out/'root.ext4'
subprocess.run(['dd',f'if={system}',f'of={root}','bs=4M','iflag=skip_bytes,count_bytes',f"skip={part['start']*512}",f"count={part['size']*512}",'conv=sparse','status=none'],check=True)
changes=[(probe_dir/'dm-block-probe.elf','/usr/bin/dm-block-probe',0o100755),(build/'system/lib/libreliefos.so.2','/usr/lib/reliefos/libreliefos.so.2',0o100755),(build/'userland/diskmgr.elf','/usr/lib/reliefos/apps/diskmgr/diskmgr.elf',0o100755),(build/'generated/nls/zh_CN/LC_MESSAGES/leonos.mo','/usr/share/locale/zh_CN/LC_MESSAGES/leonos.mo',0o100644),(W/'userland/apps/diskmgr/diskmgr.desktop','/usr/share/applications/reliefos-diskmgr.desktop',0o100644),(probe_dir/'dm-x11-probe.elf','/usr/bin/dm-x11-probe',0o100755)]
with (out/'prepare.log').open('w') as log:
 for src,dest,mode in changes:
  subprocess.run(['debugfs','-w','-R',f'rm {dest}',root],stdout=log,stderr=subprocess.STDOUT)
  for command in (f'write {src} {dest}',f'set_inode_field {dest} mode 0{mode:o}',f'set_inode_field {dest} uid 0',f'set_inode_field {dest} gid 0'):
   subprocess.run(['debugfs','-w','-R',command,root],check=True,stdout=log,stderr=subprocess.STDOUT)
 checked=subprocess.run(['e2fsck','-fn',root],stdout=log,stderr=subprocess.STDOUT)
 assert checked.returncode==0,checked.returncode
subprocess.run(['dd',f'if={root}',f'of={system}','bs=4M','oflag=seek_bytes',f"seek={part['start']*512}",'conv=notrunc,sparse','status=none'],check=True);root.unlink()
data=out/'data.raw'
with data.open('xb') as f:f.truncate(512*1024**2)
subprocess.run(['sfdisk',data],input='label: gpt\nunit: sectors\n\nstart=2048,size=262144,type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7,name="资料盘"\n',text=True,check=True,stdout=subprocess.DEVNULL)
fat=out/'data.fat'
with fat.open('xb') as f:f.truncate(128*1024**2)
subprocess.run(['mkfs.fat','-F','32','-n','QADATA',fat],check=True,stdout=subprocess.DEVNULL)
subprocess.run(['dd',f'if={fat}',f'of={data}','bs=4M','oflag=seek_bytes','seek=1048576','conv=notrunc,sparse','status=none'],check=True);fat.unlink()
identity={'runtime_sha256':digest(changes[1][0]),'template':str(source),'template_sha256':digest(source),'app_sha256':digest(changes[2][0]),'catalog_sha256':digest(changes[3][0]),'data_initial_sha256':digest(data)}
(out/'sources.json').write_text(json.dumps(identity,indent=2)+'\n')
esp=next(p for p in table['partitions'] if p['type'].lower()=='c12a7328-f81f-11d2-ba4b-00a0c93ec93b')
medium=str(system)+'@@'+str(esp['start']*512)
subprocess.run(['mcopy','-o','-i',medium,str(build/'generated/system/kernel.sys'),'::/RELIEFOS/KERNEL.SYS'],check=True)
subprocess.run(['mcopy','-o','-i',medium,str(build/'generated/system/kernel.sys'),'::/LEONOS/KERNEL.SYS'],check=True)
identity['kernel_sha256']=digest(build/'generated/system/kernel.sys')
(out/'sources.json').write_text(json.dumps(identity,indent=2)+'\n')
print('PREPARED '+str(out),flush=True)
state={'geometry':None,'app':False}
with boot(out/'boot','uefi',disk=system,extra_disk=data) as (probe,serial,process):
 unchecked_frame=probe.frame
 def checked_frame(name):
  status=probe.command('query-status')
  if not status['running']:
   (out/'qemu-status.json').write_text(json.dumps(status,indent=2)+'\n')
   raise RuntimeError('QEMU stopped before screenshot: '+str(status))
  return unchecked_frame(name)
 probe.frame=checked_frame
 def login(user):
  wait_xdm(probe,process,'login-'+user,180)
  crop,left,top=greeter_crop(probe.frame('login-panel'))
  box=_bright_panel_bbox(crop)
  if box:probe.click(left+(box[0]+box[2])//2,top+(box[1]+box[3])//2)
  probe.text(user);probe.key('ret')
  wait_xdm(probe,process,'password-'+user,60,expected='password')
  probe.text('r' if user=='root' else USER_PASSWORD);probe.key('ret')
  deadline=time.monotonic()+90
  while time.monotonic()<deadline:
   if _session_st_visible(probe.frame('desktop-'+user)):break
   time.sleep(.5)
  else:raise AssertionError('Session terminal did not appear')
  time.sleep(2);state['app']=False
 def terminal(command,expected=None,restore=True):
  start=len(serial.read_text(errors='replace'))
  if state['app']:probe.key('alt-tab');time.sleep(1)
  time.sleep(1);probe.text('  '+command);probe.key('ret')
  if expected:wait_for(serial,process,lambda s:expected in s[start:],expected,90)
  else:time.sleep(1)
  if state['app'] and restore:probe.key('alt-tab');time.sleep(1)
  return serial.read_text(errors='replace')[start:]
 def geometry():
  text=terminal('dm-x11-probe > /dev/ttyS0','DM_GEOMETRY')
  matches=re.findall(r'DM_GEOMETRY (\d+) (-?\d+) (-?\d+) (\d+) (\d+)',text)
  assert matches,text[-3000:]
  state['geometry']=list(map(int,matches[-1][1:]));return state['geometry']
 def point(x,y,kind):
  if kind=='absolute':return x,y
  gx,gy,gw,gh=state['geometry']
  if kind=='dialog':return gx+(gw-570)//2+x,gy+(gh-390)//2+y
  if kind=='password':return gx+(gw-500)//2+x,gy+(gh-196)//2+y
  return gx+x,gy+y
 login('root')
 terminal('chmod 666 /dev/ttyS0; echo DM_SERIAL_READY > /dev/ttyS0','DM_SERIAL_READY')
 terminal('LC_ALL=zh_CN.UTF-8 diskmgr > /dev/ttyS0 2>&1 &','FLTK disk manager ready')
 state['app']=True
 wait_for(serial,process,lambda s:'job complete action=0 status=0' in s,'initial scan',90)
 geometry();probe.frame('initial-diskmgr')
 print('READY '+json.dumps(state),flush=True)
 for line in sys.stdin:
  try:
   request=json.loads(line)
   with (out/'actions.jsonl').open('a') as f:f.write(json.dumps(request,ensure_ascii=False)+'\n')
   start=len(serial.read_text(errors='replace'))
   if request.get('op')=='quit':break
   if request.get('op')=='login':login(request['user']);result='logged in'
   elif request.get('op')=='terminal':result=terminal(request['command'],request.get('expected'),request.get('restore',True))[-2500:]
   elif request.get('op')=='geometry':result=geometry()
   elif request.get('op')=='state':state.update(request['value']);result=state
   elif request.get('op')=='hash':result={'data_sha256':digest(data),'initial':identity['data_initial_sha256']}
   elif request.get('op')=='table':result=json.loads(subprocess.check_output(['sfdisk','--json',data]))
   else:
    for item in request.get('items',[]):
     if 'click' in item:probe.click(*point(*item['click'],item.get('coords','client')));time.sleep(.3)
     elif 'key' in item:probe.key(item['key'])
     elif 'text' in item:probe.text(item['text'])
     elif 'password' in item:probe.text(USER_PASSWORD if item['password']=='user' else 'r')
     elif 'sleep' in item:time.sleep(item['sleep'])
    if 'job' in request:
     pattern=rf'job complete action={request["job"]} status=(-?\d+) disks=(\d+) partitions=(\d+)'
     text=wait_for(serial,process,lambda s:re.search(pattern,s[start:]) is not None,'actual disk job',180)
     match=re.search(pattern,text[start:]);assert int(match[1])==request.get('status',0),match[0]
     result=match[0]
    elif 'wait' in request:
     result=wait_for(serial,process,lambda s:request['wait'] in s[start:],'requested marker',180)[start:][-3000:]
    else:result='done'
   if request.get('frame'):probe.frame(request['frame'])
   print('RESULT '+json.dumps(result,ensure_ascii=False),flush=True)
  except Exception as error:
   probe.frame('command-error');print('ERROR '+repr(error),flush=True);traceback.print_exc()
 print('FINAL_TEMPLATE_UNCHANGED '+str(digest(source)==identity['template_sha256']),flush=True)
