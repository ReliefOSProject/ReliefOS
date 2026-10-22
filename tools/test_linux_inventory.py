#!/usr/bin/env python3
"""Test kernel inventory parsers, then optionally the supplied unmodified Fastfetch in QEMU."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
from package_fastfetch import BINARY_SHA256

ROOT=Path(__file__).resolve().parents[1]
WORK=ROOT/'build/linux-inventory'

def run(command,**kwargs):
    return subprocess.run([str(x) for x in command],cwd=ROOT,check=True,**kwargs)

def host():
    executable=WORK/'inventory-host'
    run(['cc','-std=c11','-O1','-g','-fsanitize=address,undefined','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
         '-Iinclude','-Ikernel/reliefnt/include','-Ikernel/reliefnt/include/uapi',
         '-Ikernel/reliefnt/include','tools/tests/linux_inventory_test.c','-o',executable])
    run([executable])

def guest(args):
    from make_live_root import make_live_tree
    from make_ext2_root import write_ext2_root
    import test_linux_ioctl_cloexec as iso_tools
    release_dir = args.release_dir.resolve() if args.release_dir else None
    if release_dir:
        source_tree = release_dir/'stage/live-root'
        binary = source_tree/'usr/bin/fastfetch'
        compiler = shutil.which('clang')
        assert compiler, 'clang is required to compile the inventory guest probe'
        compiler_args = [compiler, '--target=x86_64-linux-musl',
                         f'--sysroot={release_dir}/sysroot/musl']
    else:
        source_tree = ROOT/'build/esp'
        binary = source_tree/'usr/bin/fastfetch'
        compiler = ROOT/'build/musl-gcc/root/opt/dyne/gcc-musl/bin/x86_64-linux-musl-gcc'
        compiler_args = [compiler]
    binary=binary.resolve()
    digest=hashlib.sha256(binary.read_bytes()).hexdigest()
    assert digest==BINARY_SHA256, f'The packaged verification binary changed: {digest}'
    probe=WORK/'linux-inventory.elf'
    run([*compiler_args,'-static','-O2','-Wall','-Wextra','tools/tests/fastfetch_guest_probe.c','-o',probe])
    image=WORK/'root.ext2'
    with tempfile.TemporaryDirectory(prefix='stage-',dir=WORK) as directory:
        stage=Path(directory)
        make_live_tree(source_tree,stage)
        target=stage/'usr/bin/fastfetch'
        assert hashlib.sha256(target.read_bytes()).hexdigest()==digest
        target=stage/'usr/lib/reliefos/tests/linux-inventory.elf'; target.parent.mkdir(parents=True,exist_ok=True); shutil.copy2(probe,target)
        write_ext2_root(stage,image)
    iso_tools.GRUB_TEMPLATE=iso_tools.GRUB_TEMPLATE.replace('autospawn=ioctlcloexec autospawn=python315','autospawn=inventory').replace('syscall-trace=/opt/python/','').replace('ReliefOS ioctl CLOEXEC regression','ReliefOS inventory verification')
    if release_dir:
        iso=WORK/'reliefos-fastfetch.iso'
        grub_config=WORK/'grub.cfg'
        grub_config.write_text(iso_tools.GRUB_TEMPLATE, encoding='ascii')
        run([sys.executable,'tools/make_installer_iso.py','--out',str(iso.relative_to(ROOT)),
             '--build-output',str(release_dir),
             '--grub-efi-dir',iso_tools.grub_efi_dir(release_dir),
             '--stage',str((WORK/'iso').relative_to(ROOT)),
             '--boot-image',str((WORK/'efiboot.img').relative_to(ROOT)),
             '--kernel',str((release_dir/'generated/system/kernel.sys').relative_to(ROOT)),
             '--installer-root',str(image.relative_to(ROOT)),
             '--grub-font',str((release_dir/'generated/grub/leonos-unicode.pf2').relative_to(ROOT)),
             '--work-dir',str(WORK.relative_to(ROOT)),
             '--grub-config',str(grub_config.relative_to(ROOT))])
    else:
        iso=iso_tools.build_iso(image,WORK/'reliefos-fastfetch.iso',WORK/'grub.cfg',WORK)
    serial=WORK/'guest-serial.log'; serial.write_text('')
    stderr=WORK/'qemu.log'
    with tempfile.TemporaryDirectory(prefix='reliefos-inventory-') as directory, stderr.open('w') as errors:
        qmp=Path(directory)/'qmp.sock'
        process=subprocess.Popen(['qemu-system-x86_64','-enable-kvm','-cpu','host','-machine','q35','-m','4096',
            '-smp','2,sockets=1,cores=2,threads=1','-bios','/usr/share/edk2/x64/OVMF.4m.fd',
            '-smbios','type=1,manufacturer=ReliefOS-Test,product=Inventory-Machine,version=1,uuid=00112233-4455-6677-8899-aabbccddeeff',
            '-display','none','-serial',f'file:{serial}','-device','VGA,xres=1280,yres=720','-device','qemu-xhci',
            '-device','usb-tablet','-netdev','user,id=net0','-device','e1000,netdev=net0',
            '-cdrom',str(iso),'-boot','d','-qmp',f'unix:{qmp},server=on,wait=off','-no-reboot','-no-shutdown'],
            cwd=ROOT,stdout=subprocess.DEVNULL,stderr=errors)
        deadline=time.monotonic()+args.timeout
        try:
            while time.monotonic()<deadline:
                text=serial.read_text(errors='replace')
                if '[inventory] DONE' in text or 'KERNEL PANIC' in text or process.poll() is not None: break
                time.sleep(.5)
        finally:
            if process.poll() is None: iso_tools.qmp_quit(qmp,process)
    text=serial.read_text(errors='replace')
    text=re.sub(r'^\[\s*\d+\.\d+\] ?', '', text, flags=re.M)
    assert '[inventory] DONE failures=0' in text, f'Guest regression failed: {serial}'
    default_output=re.search(r'\[inventory\] BEGIN default-output\n(.*?)\n\[inventory\] END default-output',text,re.S)
    assert default_output and ':-------:.' in default_output[1], 'default ReliefOS logo missing'
    assert '00112233-4455-6677-8899-aabbccddeeff' in text, 'SMBIOS UUID endian mismatch'
    # The current kernel deliberately disables AP scheduling. Validate actual
    # admitted CPUs and managed pages, not QEMU's configured hardware capacity.
    cpu_match=re.search(r'SMP topology CPUs=(\d+)',text)
    memory_match=re.search(r'\[(?:reliefnt|ntclks)\] mm initialized usable=(\d+) KiB',text)
    assert cpu_match and memory_match, 'missing independent kernel inventory'
    enabled_cpus=int(cpu_match[1]); managed_memory=int(memory_match[1])*1024
    kernel_match=re.search(r'\[inventory\] uname release=(\S+) version=([^\n]+)',text)
    assert kernel_match, 'missing native uname identity'
    assert f'Kernel: ReliefNT {kernel_match[1]}' in default_output[1], 'default Kernel row missing'
    assert 'OS: ReliefOS' in default_output[1] and 'Memory:' in default_output[1], 'default ReliefOS information modules missing'
    modes={tuple(map(int,m)) for m in re.findall(r'framebuffer[^\n]*?(\d+)x(\d+)',text)}
    for label in ('console-json','terminal-json'):
        match=re.search(r'\[inventory\] BEGIN '+label+r'\n(.*?)\n\[inventory\] END '+label,text,re.S)
        assert match, label
        data=json.loads(match[1]); (WORK/f'{label}.json').write_text(json.dumps(data,indent=2)+'\n')
        modules={item['type']:item for item in data}
        for name in ('OS','Host','Kernel','Uptime','CPU','Memory','Swap','Disk','Display','GPU'):
            assert 'result' in modules[name], (name,modules[name])
        assert modules['Host']['result']['name']=='Inventory-Machine', modules['Host']
        assert modules['Kernel']['result']['name']=='ReliefNT', modules['Kernel']
        assert modules['Kernel']['result']['release']==kernel_match[1], modules['Kernel']
        assert modules['OS']['result']['name']=='ReliefOS', modules['OS']
        assert modules['OS']['result']['prettyName']=='ReliefOS', modules['OS']
        assert modules['Kernel']['result']['version']==kernel_match[2], modules['Kernel']
        assert modules['CPU']['result']['cores']['online']==enabled_cpus, modules['CPU']
        assert modules['Memory']['result']['total']==managed_memory,modules['Memory']
        assert modules['GPU']['result'][0]['vendor']=='QEMU',modules['GPU']
        display=modules['Display']['result'][0]['output']
        assert (display['width'],display['height']) in modes,modules['Display']
        if label=='terminal-json':
            assert modules['Shell']['result']['exeName']=='sh',modules['Shell']
            assert modules['Terminal']['result']['exePath']=='/usr/lib/reliefos/apps/terminal/terminal.elf',modules['Terminal']
    (WORK/'evidence.txt').write_text(f'Provided Fastfetch SHA256 {digest}\nHost ASan/UBSan inventory: PASS\nQEMU/KVM 2 configured CPUs, {enabled_cpus} enabled by kernel: PASS\nVMware: not run\nISO {iso}\n')
    print(f'PASS unmodified Fastfetch; evidence: {WORK}')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--guest',action='store_true')
    parser.add_argument('--timeout',type=float,default=240)
    parser.add_argument('--release-dir',type=Path,help='Makefile output directory, for example out/x86_64/release')
    args=parser.parse_args(); WORK.mkdir(parents=True,exist_ok=True)
    host()
    if args.guest: guest(args)
if __name__=='__main__': main()
