#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise DRM in QEMU or build a VMware SVGA II rendering probe ISO."""
import argparse
import json
import os
import re
from pathlib import Path
import shutil
import socket
import subprocess
import time
from qemu_rfb import QemuRfb

ROOT=Path(__file__).resolve().parents[1]


def copy_runtime(root,sources,seeds):
    """Copy only ELF dependencies, preserving guest absolute symlink targets."""
    pending=list(seeds);visited=set()
    while pending:
        name=pending.pop()
        if name in visited:continue
        visited.add(name)
        candidates=[(source,source/directory/name) for source in sources
            for directory in ("usr/lib","lib","usr/lib/dri","usr/lib/gbm",
                              "usr/libexec","usr/bin","usr/lib/reliefos",
                              "usr/lib/reliefos/apps/fileman")]
        match=next(((source,path) for source,path in candidates if path.exists() or path.is_symlink()),None)
        source,path=match if match else (None,None)
        if path is None and name=="libc.musl-x86_64.so.1":
            # The interpreter supplies musl's SONAME, just as in the staged OS.
            if any((source/"lib/ld-musl-x86_64.so.1").exists() for source in sources):continue
        if path is None:raise FileNotFoundError("missing runtime library "+name)
        destination=root/path.relative_to(source);destination.parent.mkdir(parents=True,exist_ok=True)
        if destination.exists() or destination.is_symlink():continue
        if path.is_symlink():
            target=os.readlink(path)
            destination.symlink_to(target)
            pending.append(Path(target).name)
            continue
        shutil.copy2(path,destination)
        dynamic=subprocess.check_output(["readelf","-d",str(path)],text=True)
        pending+=re.findall(r"\(NEEDED\).*?\[(.*?)\]",dynamic)


def copy_desktop(root,sources):
    """Stage the actual Xorg, IceWM and Fileman with their own data and modules."""
    source=sources[-1]
    dependencies=[]
    for directory in ("usr/lib/xorg/modules","usr/share/X11","usr/share/icewm",
                      "usr/share/fonts","usr/share/fontconfig","etc/fonts",
                      "usr/share/reliefos","usr/share/locale","etc/icewm",
                      "usr/lib/gdk-pixbuf-2.0"):
        path=source/directory
        if not path.exists():continue
        shutil.copytree(path,root/directory,symlinks=True,dirs_exist_ok=True)
        for member in path.rglob("*"):
            if not member.is_file() or member.is_symlink():continue
            with member.open("rb") as stream:elf=stream.read(4)==b"\x7fELF"
            if elf:
                dynamic=subprocess.check_output(["readelf","-d",str(member)],text=True)
                dependencies+=re.findall(r"\(NEEDED\).*?\[(.*?)\]",dynamic)
    copy_runtime(root,sources,["Xorg","icewm","xkbcomp","fileman.elf"]+dependencies)
    (root/"etc/X11").mkdir(parents=True,exist_ok=True)
    shutil.copy2(ROOT/"system/xorg/xorg.conf",root/"etc/X11/xorg.conf")
    shutil.copy2(ROOT/"system/xorg/xorg-tty-wrapper",root/"xorg-wrapper")
    (root/"xorg-wrapper").chmod(0o755)
    (root/"usr/bin/Xorg").symlink_to("../libexec/Xorg")
    for directory in ("var/log","root","run/user/0"):(root/directory).mkdir(parents=True,exist_ok=True)
    (root/"etc/passwd").write_text("root:x:0:0:root:/root:/bin/sh\n")
    (root/"etc/group").write_text("root:x:0:\n")
    return """
export PATH=/bin:/usr/bin:/usr/sbin:/sbin HOME=/root USER=root LOGNAME=root
export LANG=C.UTF-8 DISPLAY=:0 XDG_RUNTIME_DIR=/run/user/0
/xorg-wrapper :0 vt1 -keeptty -ac -nolisten tcp -logfile /var/log/Xorg.0.log </dev/tty1 >/run/xorg.log 2>&1 &
xorg=$!
for attempt in $(seq 1 80); do
 [ -S /tmp/.X11-unix/X0 ] && break
 kill -0 $xorg || break
 sleep .1
done
/usr/bin/icewm >/run/icewm.log 2>&1 &
wm=$!
sleep 1
/usr/lib/reliefos/apps/fileman/fileman.elf >/run/fileman.log 2>&1 &
app=$!
sleep 3
cat /run/xorg.log /var/log/Xorg.0.log /run/icewm.log /run/fileman.log
if ! kill -0 $xorg || ! kill -0 $wm || ! kill -0 $app; then
 echo VIRTGPU_QEMU_DONE:status=1
 poweroff -f
fi
echo VIRTGPU_DESKTOP_READY
read resume </dev/ttyS0
kill $app $wm
kill $xorg
wait $xorg
echo VIRTGPU_CONSOLE_RESTORED >/dev/tty1
sleep .2
echo VIRTGPU_DESKTOP_STOPPED
read resume </dev/ttyS0
"""


def connect(path,process,deadline):
    while time.monotonic()<deadline:
        if process.poll() is not None:raise RuntimeError("QEMU exited before socket connection")
        stream=socket.socket(socket.AF_UNIX)
        try:
            stream.connect(str(path));return stream
        except (FileNotFoundError,ConnectionRefusedError):
            stream.close();time.sleep(.05)
    raise TimeoutError("QEMU socket connection")


def inject_panic(output,symbols,screenshot):
    """Inject a real fault only into this VM's RAM, freezing at panic entry."""
    ready=output/"panic-frozen";resume=output/"panic-resume"
    script=output/"panic.gdb"
    commands=["set pagination off","set confirm off",f"target remote {output/'gdb.sock'}",
        "python",
        "import gdb, time",
        "for thread in gdb.selected_inferior().threads():",
        "    thread.switch()",
        "    if (int(gdb.parse_and_eval('$cs')) & 3) == 0: break",
        "else: raise gdb.GdbError('no stopped kernel CPU')",
        "root = int(gdb.parse_and_eval('$cr3')) & 0x000ffffffffff000",
        "base = 0xffff800000000000",
        "target = None",
        "for slot in range(256, 511):",
        "    entry = bytes(gdb.selected_inferior().read_memory(base + root + slot * 8, 8))",
        "    if not (int.from_bytes(entry, 'little') & 1):",
        "        target = 0xffff000000000000 | (slot << 39)",
        "        break",
        "if target is None: raise gdb.GdbError('no absent kernel PML4 slot')",
        "gdb.execute('set $fault_target = (unsigned long long)0x%x' % target)",
        "print('FAULT-TARGET=0x%016X PML4E=not-present' % target)",
        "pc = int(gdb.parse_and_eval('(unsigned long)&vg_display_refresh'))",
        "print('FAULT-PC=0x%X CS=%#x' % (pc, int(gdb.parse_and_eval('$cs'))))",
        "gdb.selected_inferior().write_memory(pc, bytes([0x48, 0x8b, 0x00]))",
        "gdb.execute('set $rip = (unsigned long long)0x%x' % pc)",
        "end",
        "set $rax = $fault_target","hbreak console_panic_begin","continue",
        "python","from pathlib import Path",f"Path({str(ready)!r}).touch()",
        "deadline = time.monotonic()+20",
        f"while not Path({str(resume)!r}).exists():",
        "    if time.monotonic()>deadline: raise gdb.GdbError('host capture timeout')",
        "    time.sleep(.05)",
        "end","delete breakpoints","detach","quit"]
    script.write_text("\n".join(commands)+"\n")
    with (output/"panic-gdb.log").open("w") as log:
        debugger=subprocess.Popen(["gdb","-q","-nx","--batch",str(symbols),"-x",str(script)],stdout=log,stderr=log)
        try:
            deadline=time.monotonic()+25
            while not ready.exists():
                if debugger.poll() is not None:raise RuntimeError("GDB failed before panic entry")
                if time.monotonic()>deadline:raise TimeoutError("fault injection")
                time.sleep(.05)
            screenshot("before-panic.ppm");resume.touch()
            if debugger.wait(timeout=10):raise RuntimeError("GDB fault injection failed")
        finally:
            if debugger.poll() is None:debugger.terminate();debugger.wait(timeout=5)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("kernel","busybox","output"):
        parser.add_argument("--"+name,type=Path,required=True)
    parser.add_argument("--probe", type=Path, help="KMS probe (optional with --svga3d)")
    backend = parser.add_mutually_exclusive_group()
    backend.add_argument("--svga2", action="store_true", help="run the basic VMware SVGA II probe in QEMU")
    backend.add_argument("--svga3d", choices=("legacy", "gb", "dx"),
                         help="build a VMware ISO for the selected Mesa SVGA3D path")
    parser.add_argument("--gl",action="store_true")
    parser.add_argument("--uefi",action="store_true")
    parser.add_argument("--desktop",action="store_true")
    parser.add_argument("--panic-mode",choices=("gui","tty"))
    parser.add_argument("--symbols",type=Path)
    parser.add_argument("--egl-probe",type=Path)
    parser.add_argument("--bench-draws", type=int, default=1000,
                        help="draw count for the offscreen EGL probe")
    parser.add_argument("--vmware-console-hold", type=int, default=0, metavar="SECONDS",
                        help="publish a text-console marker after KMS and wait for a host capture")
    parser.add_argument("--runtime-root",type=Path)
    parser.add_argument("--library-overlay",type=Path,action="append",default=[])
    parser.add_argument("--module-root",type=Path,help="modules_install root from the tested kernel build")
    parser.add_argument("--timeout",type=float,default=90)
    args=parser.parse_args()
    if args.svga2 and (args.gl or args.desktop or args.egl_probe):
        parser.error("SVGA II mode uses the basic 2D probe")
    if args.vmware_console_hold and (not args.svga3d or not args.probe or
                                     not 1 <= args.vmware_console_hold <= 60):
        parser.error("--vmware-console-hold requires --svga3d/--probe and 1–60 seconds")
    if args.svga3d:
        if any((args.gl, args.uefi, args.desktop, args.panic_mode, args.module_root)):
            parser.error("--svga3d prepares a VMware 3D image without QEMU options")
        if not args.egl_probe or not args.runtime_root:
            parser.error("--svga3d requires --egl-probe and --runtime-root")
    elif not args.probe:
        parser.error("QEMU verification requires --probe")
    if args.egl_probe and (not args.runtime_root or not (args.gl or args.svga3d)):
        parser.error("EGL probe requires --runtime-root and either --gl or --svga3d")
    if not 1 <= args.bench_draws <= 1000000:
        parser.error("--bench-draws must be in [1, 1000000]")
    marker = "SVGA" if args.svga2 else "VIRTGPU"
    done_pattern = re.compile(rf"^{marker}_QEMU_DONE:status=(\d+)\r?\n", re.MULTILINE)
    if args.panic_mode and (not args.desktop or not args.symbols):
        parser.error("panic verification requires --desktop and --symbols")
    for name in ("kernel","busybox","output"):
        setattr(args,name,getattr(args,name).resolve())
    if args.output.exists() or not args.output.is_relative_to(ROOT/"out"):
        parser.error("output must be a new directory inside out")
    root=args.output/"root"
    for directory in ("bin","dev","proc","sys","tmp","run","etc"):
        (root/directory).mkdir(parents=True,exist_ok=True)
    shutil.copy2(args.busybox,root/"bin/busybox")
    for applet in subprocess.check_output([str(args.busybox),"--list"],text=True).splitlines():
        if applet != "busybox":
            (root/"bin"/applet).symlink_to("busybox")
    (root/"sbin").symlink_to("bin")
    if args.probe:
        shutil.copy2(args.probe,root/"probe")
    modules_before=modules_after=""
    if args.module_root:
        module_tree=args.module_root.resolve()/"lib/modules"
        if not list(module_tree.glob("*/kernel/drivers/video/drm/drm.ko")):
            parser.error("module-root must contain the tested drm.ko and depmod output")
        shutil.copytree(module_tree,root/"lib/modules",symlinks=True)
        modules_before="modprobe drm || fatal\ngrep '^drm ' /proc/modules || fatal\n"
        modules_after="""
[ $status = 0 ] || fatal
rmmod drm || fatal
[ ! -e /dev/dri/card0 ] && [ ! -e /dev/dri/renderD128 ] || fatal
if grep '^drm ' /proc/modules; then fatal; fi
echo VIRTGPU_MODULE_UNLOADED
modprobe drm || fatal
/probe --no-pause || fatal
rmmod drm || fatal
[ ! -e /dev/dri/card0 ] || fatal
echo VIRTGPU_MODULE_PASS load=pass unload=pass reload=pass
"""
    if args.egl_probe:
        shutil.copy2(args.egl_probe,root/"egl-probe")
        dri = "vmwgfx_dri.so" if args.svga3d else "virtio_gpu_dri.so"
        copy_runtime(root,[p.resolve() for p in args.library_overlay]+[args.runtime_root.resolve()],
            ["libEGL.so.1","libGLESv2.so.2","libgbm.so.1","libdril_dri.so",
             dri,"dri_gbm.so","ld-musl-x86_64.so.1"])
    egl=f"GPU_PROBE_DRAWS={args.bench_draws} /egl-probe\nstatus=$?\nif [ $status != 0 ]; then echo VIRTGPU_QEMU_DONE:status=$status; poweroff -f; fi\n" if args.egl_probe else ""
    if args.desktop and not args.runtime_root:parser.error("desktop requires --runtime-root")
    desktop=copy_desktop(root,[p.resolve() for p in args.library_overlay]+[args.runtime_root.resolve()]) if args.desktop else ""
    (root/"init").write_text("""#!/bin/sh
export PATH=/bin
exec </dev/console >/dev/ttyS0 2>&1
fatal() { echo VIRTGPU_QEMU_DONE:status=1; poweroff -f; while :; do sleep 1; done; }
"""+modules_before+egl+desktop+"""/probe
status=$?
"""+modules_after+"""echo VIRTGPU_QEMU_DONE:status=$status
cat /proc/interrupts
poweroff -f
while :; do sleep 1; done
""")
    if args.svga3d:
        host_backed = int(args.svga3d == "legacy")
        vgpu10 = int(args.svga3d == "dx")
        kms = 'if [ "$status" = 0 ]; then /probe --vmware; status=$?; fi\n' if args.probe else ""
        console = ""
        if args.vmware_console_hold:
            console = f"""if [ "$status" = 0 ]; then
 echo SVGA CONSOLE RESTORED >/dev/tty1
 echo SVGA_CONSOLE_CAPTURE_READY
 sleep {args.vmware_console_hold}
fi
"""
        (root/"init").write_text(f"""#!/bin/sh
export PATH=/bin
exec </dev/console >/dev/ttyS0 2>&1
# Set the same winsys switches used by the upstream Mesa driver.
export SVGA_FORCE_HOST_BACKED={host_backed} SVGA_VGPU10={vgpu10}
GPU_PROBE_DRAWS={args.bench_draws} /egl-probe {args.svga3d}
status=$?
{kms}{console}echo SVGA3D_VMWARE_DONE:status=$status
poweroff -f
while :; do sleep 1; done
""")
    elif args.svga2:
        init = root/"init"
        init.write_text(init.read_text().replace("VIRTGPU_", "SVGA_"))
    (root/"init").chmod(0o755)
    boot=args.output/"iso/boot";(boot/"grub").mkdir(parents=True)
    names=["."]+sorted(str(path.relative_to(root)) for path in root.rglob("*"))
    with (boot/"initramfs.cpio").open("wb") as stream:
        subprocess.run(["cpio","--null","-o","--format=newc","--owner=0:0"],
            input=("\0".join(names)+"\0").encode(),cwd=root,stdout=stream,check=True)
    shutil.copy2(args.kernel,boot/"kernel.sys")
    shutil.copy2("/usr/share/grub/unicode.pf2",boot/"grub/unicode.pf2")
    title = "VMware SVGA II" if args.svga2 or args.svga3d else "VirtIO-GPU"
    (boot/"grub/grub.cfg").write_text(f"""set timeout=0
serial --unit=0 --speed=115200
terminal_output serial
insmod all_video
insmod gfxterm
loadfont /boot/grub/unicode.pf2
set gfxmode=1024x768x32
set gfxpayload=keep
menuentry "ReliefOS {title} test" {{
 multiboot2 /boot/kernel.sys root=/ log=serial,screen rdinit=/init drm.faildiag=1
 module2 /boot/initramfs.cpio initramfs
 boot
}}
""")
    iso=args.output/"qa.iso"
    with (args.output/"iso.log").open("w") as log:
        subprocess.run(["grub-mkrescue","-o",str(iso),str(boot.parent)],stdout=log,stderr=log,check=True)
    if args.svga3d:
        vmx = args.output/"svga.vmx"
        # Workstation exposes host-backed SVGA3D with hardware version 9.
        hardware_version = 9 if args.svga3d == "legacy" else 20
        vmx.write_text(f""".encoding = "UTF-8"
config.version = "8"
virtualHW.version = "{hardware_version}"
displayName = "ReliefOS SVGA II {args.svga3d} test"
guestOS = "other-64"
firmware = "bios"
memsize = "2048"
numvcpus = "2"
ide1:0.present = "TRUE"
ide1:0.deviceType = "cdrom-image"
ide1:0.fileName = "qa.iso"
ide1:0.startConnected = "TRUE"
serial0.present = "TRUE"
serial0.fileType = "file"
serial0.fileName = "serial.log"
serial0.startConnected = "TRUE"
mks.enable3d = "TRUE"
ethernet0.present = "FALSE"
""")
        print(vmx)
        return
    qmp_path=args.output/"qmp.sock";serial_path=args.output/"serial.sock"
    command=["qemu-system-x86_64","-machine","q35","-accel","kvm","-cpu","host","-m","4096" if args.desktop else "2048",
        "-smp","4","-vga","none","-device", "vmware-svga" if args.svga2 else ("virtio-vga-gl" if args.gl else "virtio-vga"),
        "-display","egl-headless,rendernode=/dev/dri/renderD128" if args.gl else "none",
        "-serial",f"unix:{serial_path},server=on,wait=on",
        "-qmp",f"unix:{qmp_path},server=on,wait=off","-nic","none","-cdrom",str(iso),
        "-boot","d","-no-reboot"]
    if args.uefi:command += ["-bios","/usr/share/ovmf/x64/OVMF.4m.fd"]
    if args.gl:command += ["-vnc",f"unix:{args.output/'vnc.sock'}"]
    if args.panic_mode:command += ["-chardev",f"socket,path={args.output/'gdb.sock'},server=on,wait=off,id=gdb0",
        "-gdb","chardev:gdb0","-no-shutdown"]
    result={"passed":False,"command":command,"vmware_verified":False}
    with (args.output/"qemu.log").open("w") as log:
        process=subprocess.Popen(command,stdout=log,stderr=log,env={**os.environ,"TMPDIR":str(ROOT/"out/tmp")})
        try:
            deadline=time.monotonic()+args.timeout
            serial=connect(serial_path,process,deadline);serial.settimeout(.2)
            qmp=connect(qmp_path,process,deadline);qmp.settimeout(10)
            qmp_file=qmp.makefile("rwb",buffering=0)
            json.loads(qmp_file.readline())
            qmp_file.write(b'{"execute":"qmp_capabilities"}\n')
            while "return" not in json.loads(qmp_file.readline()):pass
            # GL scanouts are textures: QMP screendump has no DisplaySurface.
            # Connecting a 2D VNC listener also enables EGL-headless readback.
            rfb_stream=connect(args.output/"vnc.sock",process,deadline) if args.gl else None
            rfb=QemuRfb(rfb_stream,deadline) if rfb_stream else None
            output="";captured=False;desktop_captured=False;stopped_captured=False;injected=False
            def screenshot(name):
                if rfb:return rfb.capture(args.output/name)
                qmp_file.write((json.dumps({"execute":"screendump","arguments":{
                    "filename":str(args.output/name)}})+"\n").encode())
                while True:
                    response=json.loads(qmp_file.readline())
                    if "error" in response:raise RuntimeError(response)
                    if "return" in response:return
            with (args.output/"serial.log").open("w") as evidence:
                while time.monotonic()<deadline:
                    try:
                        data=serial.recv(65536)
                        if not data:break
                    except socket.timeout:
                        if process.poll() is not None:break
                        continue
                    chunk=data.decode(errors="replace");output+=chunk;evidence.write(chunk);evidence.flush()
                    if "VIRTGPU_DESKTOP_READY" in output and not desktop_captured:
                        screenshot("desktop.ppm");desktop_captured=True
                        if args.panic_mode=="gui":
                            inject_panic(args.output,args.symbols,screenshot);injected=True
                        else:serial.sendall(b"\n")
                    if "VIRTGPU_DESKTOP_STOPPED" in output and not stopped_captured:
                        # Serial completion and EGL/VNC readback are independent.
                        # Require the real visible marker, retaining every frame
                        # while boundedly waiting for display publication.
                        console_deadline=min(deadline,time.monotonic()+5)
                        attempt=0
                        while True:
                            attempt+=1
                            name=f"console-wait-{attempt}.ppm"
                            screenshot(name)
                            visible=subprocess.check_output(["tesseract",str(args.output/name),"stdout","--psm","6"],stderr=subprocess.DEVNULL,text=True)
                            (args.output/f"console-wait-{attempt}.txt").write_text(visible)
                            if "_CONSOLE_RESTORED" in visible:
                                shutil.copy2(args.output/name,args.output/"console.ppm")
                                break
                            if time.monotonic()>=console_deadline:
                                raise AssertionError("retained text console marker is not visible")
                        stopped_captured=True
                        result["console_capture_attempts"]=attempt
                        result["console_restored"]=True
                        if args.panic_mode=="tty":
                            inject_panic(args.output,args.symbols,screenshot);injected=True
                        else:serial.sendall(b"\n")
                    if f"{marker}_SCREENSHOT_READY" in output and not captured:
                        if rfb:
                            result["capture_size"]=screenshot("kms.ppm")
                            result["capture_transport"]="rfb"
                        else:
                            screenshot("kms.ppm")
                        captured=True
                        qmp_file.write(b'{"execute":"send-key","arguments":{"keys":[{"type":"qcode","data":"ret"}]}}\n')
                    if done_pattern.search(output):break
                    if injected and "RESTART REQUIRED !!!" in output:
                        # The last fatal flush follows the serial text. Allow it
                        # to complete, then prove both visibility and stability.
                        time.sleep(.3);screenshot("panic.ppm")
                        time.sleep(.5);screenshot("panic-stable.ppm")
                        break
            if args.panic_mode:
                from PIL import Image,ImageOps
                from test_panic_console_qemu import inspect_overlay
                if not injected or "RESTART REQUIRED" not in output:raise AssertionError("panic did not complete")
                report=output[output.index("!!! KERNEL PANIC"):]
                first=Image.open(args.output/"panic.ppm").convert("RGB")
                late=Image.open(args.output/"panic-stable.ppm").convert("RGB")
                if first.size!=late.size or first.tobytes()!=late.tobytes():raise AssertionError("panic pixels changed after halt")
                visible=subprocess.check_output(["tesseract",str(args.output/"panic.ppm"),"stdout","--psm","6"],text=True,stderr=subprocess.DEVNULL)
                if args.panic_mode=="gui":
                    # Separate the red headline from the surrounding GUI.
                    heading=first.crop((0,0,len("!!! KERNEL PANIC - SYSTEM HALTED !!!")*8,16)).getchannel("R")
                    heading=ImageOps.expand(heading.point(lambda value:0 if value>150 else 255),border=6,fill=255)
                    heading=heading.resize((heading.width*3,heading.height*3))
                    heading.save(args.output/"panic-head-ocr.png")
                    visible+=subprocess.check_output(["tesseract",str(args.output/"panic-head-ocr.png"),"stdout","--psm","7"],text=True,stderr=subprocess.DEVNULL)
                if "KERNEL PANIC" not in visible or not re.search(r"CR[2Z]",visible):raise AssertionError("panic text is not visible")
                if "Unhandled Page Fault" not in report or "PF: P=0 W=0 U=0" not in report:
                    raise AssertionError("injected non-present supervisor read did not produce the expected page fault")
                if args.panic_mode=="gui":
                    result.update(inspect_overlay(Image.open(args.output/"before-panic.ppm"),first,report))
                first.save(args.output/"panic.png")
                (args.output/"panic-report.txt").write_text(report)
                result.update(passed=True,panic_mode=args.panic_mode,stable_after_halt=True,screenshot="panic.png")
                return
            done = done_pattern.search(output)
            if not done or int(done.group(1)) != 0:
                raise AssertionError(f"{marker} guest completion status: {done.group(1) if done else 'missing'}")
            expected = [f"{marker}_KMS_PASS"]
            if not args.svga2:
                expected.append("[virtio-gpu] ready")
            for text in expected:
                if text not in output:
                    raise AssertionError("missing guest result " + text)
            if args.svga2:
                from PIL import Image
                if "SVGA_DAMAGE_PASS" not in output:raise AssertionError("atomic damage probe failed")
                with Image.open(args.output/"kms.ppm") as frame:
                    if (frame.size != (800, 600) or frame.getpixel((12, 12)) != (16, 208, 96)
                        or frame.getpixel((40, 40)) != (224, 144, 32)
                        or frame.getpixel((30, 30)) != (32, 96, 160)):
                        raise AssertionError("SVGA dirty/atomic clips or pixels outside the damage are incorrect")
            if args.egl_probe and "VIRTGPU_GL_PASS" not in output:raise AssertionError("EGL probe failed")
            if args.module_root and f"{marker}_MODULE_PASS" not in output:raise AssertionError("module reload failed")
            if not captured or f"{marker}_FAIL" in output or "Kernel Panic" in output:
                raise AssertionError("GPU guest failed")
            result.update(passed=True,screenshot="kms.ppm")
            if rfb_stream:rfb_stream.close()
        except Exception as error:
            result["error"]=repr(error);raise
        finally:
            if process.poll() is None:
                process.terminate()
                try:process.wait(timeout=5)
                except subprocess.TimeoutExpired:process.kill();process.wait()
            (args.output/"result.json").write_text(json.dumps(result,indent=2)+"\n")
    print(json.dumps(result,indent=2))


if __name__=="__main__":main()
