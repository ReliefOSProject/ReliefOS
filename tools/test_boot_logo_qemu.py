#!/usr/bin/env python3
"""Observe embedded boot logos and their removal in a private QEMU disk copy."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

from PIL import Image
from run_gcc_probe_qemu import qmp_quit
from test_vdso_qemu import gpt_partitions

ROOT = Path(__file__).resolve().parents[1]


def run(command, **kwargs):
    return subprocess.run([str(value) for value in command], cwd=ROOT,
                          check=True, **kwargs)


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def inspect_frames(before, after, cpus):
    """Check actual guest pixels, including empty spacing and reclaimed rows."""
    before, after = before.convert('RGB'), after.convert('RGB')
    if before.size != after.size or before.width < 16 + cpus * 96:
        raise AssertionError('this screenshot scenario needs one full-size logo row')
    blue = lambda p: p[2] > 40 and p[2] > p[0] + 30
    counts = []
    for index in range(cpus):
        left = 16 + index * 96
        visible = sum(blue(before.getpixel((x,y)))
                      for y in range(16,96) for x in range(left,left+80))
        remaining = sum(blue(after.getpixel((x,y)))
                        for y in range(16,96) for x in range(left,left+80))
        if visible <= 80*80//3 or remaining:
            raise AssertionError(f'logo {index}: visible={visible}, remaining={remaining}')
        counts.append(visible)
    # Black margins and the full 16-pixel gaps distinguish separate logos.
    for y in range(112):
        for x in range(before.width):
            inside = 16 <= y < 96 and any(
                16+i*96 <= x < 96+i*96 for i in range(cpus))
            if not inside and before.getpixel((x,y)) != (0,0,0):
                raise AssertionError(f'logo spacing overwritten at {x},{y}')
    first_row = sum(after.getpixel((x,y)) != (0,0,0)
                    for y in range(16) for x in range(after.width))
    if not first_row:
        raise AssertionError('the hidden-logo area was not reclaimed by text')
    return {'visible_logos':cpus,'blue_pixels_per_logo':counts,
            'gap_pixels':16,'logo_size':80,'first_text_row_pixels_after_hide':first_row}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--cpus',type=int,choices=(1,2,4),default=4)
    args=parser.parse_args()
    build,output=args.build_dir.resolve(),args.output.resolve()
    if not output.is_relative_to(ROOT/'out') or output.exists():
        parser.error('--output must be a new directory under out/')
    if len(str(output/'gdb.sock').encode()) >= 108:
        parser.error('use a short output path for Unix sockets')
    output.mkdir(parents=True)
    temporary=ROOT/'out/tmp'
    temporary.mkdir(parents=True,exist_ok=True)
    os.environ['TMPDIR']=str(temporary)
    source=build/'images/reliefos.raw'
    kernel=build/'generated/system/kernel.sys'
    symbols=build/'reliefnt/generated/system/kernel.unstripped'
    source_sha=digest(source)
    metadata={'platform':'QEMU/KVM q35, VMware SVGA II, UEFI',
              'cpus':args.cpus,'source_image':str(source),'source_sha256':source_sha,
              'kernel_sha256':digest(kernel),'vmware_verified':False,
              'observation':'GDB hardware breakpoint before/after normal removal; no kernel delay'}
    raw=output/'guest.raw'
    run(['cp','--reflink=auto','--sparse=always',source,raw])
    partitions=gpt_partitions(raw)
    esp=next(part for part in partitions if part['name']=='RELIEFOS_ESP')
    image_spec=f'{raw}@@{int(esp["first"])*512}'
    run(['mcopy','-o','-i',image_spec,kernel,'::/reliefos/kernel.sys'])
    readback=output/'shipped-kernel.sys'
    run(['mcopy','-i',image_spec,'::/reliefos/kernel.sys',readback])
    if digest(readback)!=metadata['kernel_sha256']:
        raise AssertionError('private guest did not receive the current kernel')
    metadata['private_image_kernel_verified']=True
    serial,qmp,gdb=output/'serial.log',output/'qmp.sock',output/'gdb.sock'
    command=['qemu-system-x86_64','-enable-kvm','-cpu','host','-machine','q35',
             '-m','4096','-smp',str(args.cpus),'-bios','/usr/share/edk2/x64/OVMF.4m.fd',
             '-display','none','-serial',f'file:{serial}','-vga','vmware',
             '-device','qemu-xhci','-device','usb-tablet',
             '-netdev','user,id=net0','-device','e1000,netdev=net0',
             '-drive',f'file={raw},format=raw,if=ide,snapshot=on',
             '-qmp',f'unix:{qmp},server=on,wait=off',
             '-chardev',f'socket,path={gdb},server=on,wait=off,id=gdb0',
             '-gdb','chardev:gdb0','-S','-no-reboot','-no-shutdown']
    (output/'qemu-command.json').write_text(json.dumps(command,indent=2)+'\n')
    process=None
    try:
        with (output/'qemu.log').open('w') as log:
            process=subprocess.Popen(command,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT)
        deadline=time.monotonic()+15
        while not gdb.exists() or not qmp.exists():
            if process.poll() is not None: raise RuntimeError('QEMU exited during setup')
            if time.monotonic()>deadline: raise TimeoutError('QEMU sockets not ready')
            time.sleep(.1)
        before,late,after=(output/name for name in (
            'boot-logos.ppm','boot-before-hide.ppm','boot-complete.ppm'))
        script=output/'observe.gdb'
        script.write_text('\n'.join([
            'set pagination off','set confirm off',f'target remote {gdb}',
            'hbreak svga3d_init','continue',
            f'monitor screendump {before}','delete breakpoints',
            'hbreak console_boot_complete','continue',
            f'monitor screendump {late}','finish',
            f'monitor screendump {after}','delete breakpoints','detach','quit','']))
        print(f'Booting {args.cpus} CPUs; observing real show/hide transition',flush=True)
        with (output/'gdb.log').open('w') as log:
            run(['gdb','-q','-nx','--batch',symbols,'-x',script],
                stdout=log,stderr=subprocess.STDOUT,timeout=180)
        with Image.open(before) as frame: frame.save(output/'boot-logos.png')
        with Image.open(late) as frame: frame.save(output/'boot-before-hide.png')
        with Image.open(after) as frame: frame.save(output/'boot-complete.png')
        with Image.open(before) as first,Image.open(after) as last:
            metadata.update(inspect_frames(first,last,args.cpus))
        with Image.open(late) as first,Image.open(after) as last:
            metadata['before_hide']=inspect_frames(first,last,args.cpus)
        deadline=time.monotonic()+90
        while time.monotonic()<deadline:
            text=serial.read_text(errors='replace') if serial.exists() else ''
            if re.search(r'\[(?:reliefnt|ntclks)\] PID 1 path=',text): break
            if process.poll() is not None: raise RuntimeError('guest exited before PID 1')
            time.sleep(.2)
        else: raise TimeoutError('guest did not enter PID 1')
        text=serial.read_text(errors='replace')
        if not re.search(r'boot complete:.*online_cpus='+str(args.cpus)+'/'+str(args.cpus),text):
            raise AssertionError('detected/online CPU count did not match the logos')
        if re.search(r'\b(panic|bugcheck|fatal)\b',text,re.I):
            raise AssertionError('fatal kernel log')
        metadata['pid1_prepared']=True
        metadata['passed']=True
        print('PASS actual CPU logo count, spacing, persistence, automatic removal, full text rows and PID 1 preparation',flush=True)
    except Exception as error:
        metadata.update(passed=False,error=str(error))
        raise
    finally:
        if process is not None and process.poll() is None:
            qmp_quit(qmp,process)
        metadata['source_image_unchanged']=digest(source)==source_sha
        (output/'result.json').write_text(json.dumps(metadata,indent=2)+'\n')
        if not metadata['source_image_unchanged']:
            raise AssertionError('source image changed')


if __name__=='__main__':
    main()
