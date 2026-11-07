#!/usr/bin/env python3
"""Drive an explicitly supplied disposable VGA QA VM and capture native screens.

The VM must contain the public test:test fixture account and the kernel-owned
vga_text_guest.c probe at /usr/lib/reliefos/tests/vga-text-guest. Start with
console=vga bootlog-pause=1, preserving the normal graphical GRUB handoff.
This is OS integration orchestration; all terminal implementation tests and
the native ABI probe belong to the ReliefNT repository.
"""
import argparse
import json
import os
import re
from pathlib import Path
import time

from test_installer_accounts_qemu import Probe
from test_xorg_vm_qemu import session_st_bounds
from PIL import ImageChops


class Process:
    def __init__(self,pid): self.pid=pid
    def poll(self):
        try: os.kill(self.pid,0)
        except ProcessLookupError: return 1
        return None


def wait(probe,process,check,description,timeout=240):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        if process.poll() is not None: raise RuntimeError("QA VM exited")
        result=check()
        if result: return result
        time.sleep(.4)
    probe.frame("failure-"+description)
    raise AssertionError("Timed out: "+description)


def text_cells(probe):
    target=probe.output/"vga-aperture.bin"
    probe.command("pmemsave",{"val":0xb8000,"size":4000,"filename":str(target)})
    return target.read_bytes()[::2]


def greeter(probe):
    frame=probe.frame("greeter-poll")
    channels=frame.convert("RGB").split()
    masks=[channel.point(lambda v:255 if 185<=v<=195 else 0) for channel in channels]
    mask=ImageChops.multiply(ImageChops.multiply(masks[0],masks[1]),masks[2])
    box=mask.getbbox()
    return bool(box and mask.histogram()[255]>50000 and box[2]-box[0]>=400)


def visible_text(probe,label):
    """Check rendered pixels as well as scanout size, catching an empty font."""
    frame=probe.frame(label)
    return frame if frame.size==(720,400) and sum(frame.convert("L").histogram()[64:])>1500 else None


def select_vt(probe,process,vt):
    probe.key(f"ctrl-alt-f{vt}")
    def active():
        matches=re.findall(r"VT active (\d+) graphics=",probe.serial.read_text(errors="replace"))
        return int(matches[-1])==vt if matches else vt==1
    wait(probe,process,active,f"vt{vt}-ownership")


def login_vt(probe,process,vt,*,fresh=False):
    """Authenticate a stable shell; login drafts expire after sixty seconds."""
    select_vt(probe,process,vt)
    def state():
        cells=text_cells(probe)
        shell=cells.rfind(b"test@reliefos:")
        username=cells.rfind(b"reliefos login:")
        password=cells.rfind(b"Password:")
        if shell>=0 and shell>max(username,password):return "shell"
        if password>=0 and password>username:return "password"
        return "login" if username>=0 else None
    current=wait(probe,process,state,f"tty{vt}-login")
    if current=="shell":
        if not fresh:return
        probe.text("exit");probe.key("ret")
        current=wait(probe,process,lambda:state() if state() in ("login","password") else None,f"tty{vt}-logout")
    # BusyBox init can print respawn notices below a prompt on /dev/console.
    # Match the latest login/password prompt, not the last nonblank screen row.
    for attempt in range(3):
        if current!="password":
            probe.key("ctrl-u");probe.text("test");probe.key("ret")
            try:
                wait(probe,process,lambda:state()=="password",f"tty{vt}-password",timeout=15)
            except AssertionError:
                current=wait(probe,process,state,f"tty{vt}-new-login")
                continue
        probe.text("test");probe.key("ret")
        current=wait(probe,process,lambda:state() if state() in ("shell","login") else None,f"tty{vt}-shell",timeout=120)
        if current=="shell":return
    raise AssertionError(f"TTY{vt} did not authenticate the public test fixture")


def normal_login(probe,process,vt=2):
    """Capture the stock login shell before any terminal probe can clear it."""
    offset=len(probe.serial.read_text(errors="replace"))
    login_vt(probe,process,vt,fresh=True)
    def automatic_fastfetch():
        log=probe.serial.read_text(errors="replace")[offset:]
        pids=re.findall(r"exec pid=(\d+) path=/usr/bin/fastfetch ",log)
        return any(re.search(rf"task exited pid={pid} name=fastfetch code=0\b",log) for pid in pids)
    wait(probe,process,automatic_fastfetch,"automatic-login-fastfetch",timeout=120)
    wait(probe,process,lambda:all(s in text_cells(probe) for s in
        (b"OS: ReliefOS",b"Shell: bash",b"test@reliefos:~$")),"normal-bash-prompt")
    assert visible_text(probe,"02-tty-login") is not None


def normal_shell_io(probe,process):
    """Type ordinary Bash commands and check their evaluated output."""
    for command,output in (
        ("echo SHELL_VERSION=$BASH_VERSION",b"SHELL_VERSION=5."),
        ("echo RESULT=$((84/2))",b"RESULT=42"),
        ("echo file-$((84/2)) > /tmp/tty-io; cat /tmp/tty-io",b"file-42"),
    ):
        probe.text(command);probe.key("ret")
        wait(probe,process,lambda expected=output:expected in text_cells(probe),"bash-command-output")
        wait(probe,process,lambda expected=output:
            (cells:=text_cells(probe)).rfind(b"test@reliefos:~$")>cells.rfind(expected),"bash-prompt-after-command")
    probe.frame("tty2-bash-input-output")


def desktop_login(probe,process):
    """Wait for each XDM input state after the X server reacquires its VT."""
    frame=probe.frame("xdm-login-ready")
    channels=frame.convert("RGB").split()
    masks=[c.point(lambda v:255 if 185<=v<=195 else 0) for c in channels]
    box=ImageChops.multiply(ImageChops.multiply(masks[0],masks[1]),masks[2]).getbbox()
    assert box is not None
    left,top=box[:2]
    probe.click(left+320,top+117)
    # The restored picture can precede Xorg's input reacquisition. Clear with
    # unmodified keys, so an early modifier chord cannot become a literal 'u'.
    for unused in range(8):probe.key("backspace")
    probe.text("test")
    def username_visible():
        crop=probe.frame("xdm-username").convert("L").crop((left+134,top+108,left+230,top+126))
        return sum(crop.histogram()[:64])>30
    wait(probe,process,username_visible,"xdm-username",timeout=30)
    probe.key("ret")
    def password_ready():
        crop=probe.frame("xdm-password-ready").convert("L").crop((left+130,top+142,left+455,top+143))
        return sum(crop.histogram()[:64])>200
    wait(probe,process,password_ready,"xdm-password",timeout=30)
    probe.text("test");probe.key("ret")
    wait(probe,process,lambda:session_st_bounds(probe.frame("desktop-ready")),"icewm-login",timeout=180)


def run(args):
    process=Process(args.pid)
    probe=Probe(args.qmp,args.output)
    probe.serial=args.serial
    result={"vga_requested":True,"pid":args.pid,"vmware_verified":False,"checks":[]}
    try:
        wait(probe,process,lambda:args.serial.exists() and
            "kernel_console=VGA-text" in args.serial.read_text(errors="replace"),"vga-console-selection",timeout=120)
        if not args.skip_boot:
            wait(probe,process,lambda:"Boot log paused." in args.serial.read_text(errors="replace"),"boot-pause")
            wait(probe,process,lambda:visible_text(probe,"01-kernel-boot"),"visible-boot-text",timeout=30)
            probe.key("ret")
        else:
            probe.key("ctrl-alt-f1")
        wait(probe,process,lambda:greeter(probe),"xdm",timeout=420)
        probe.frame("03-xdm")
        print("XDM visible",flush=True)
        result["checks"].append("VGA boot -> Xorg XDM")
        normal_login(probe,process,2)
        normal_shell_io(probe,process)
        result["checks"].append("stock login -> Bash -> automatic fastfetch and normal shell input/output")
        probe.text("/usr/lib/reliefos/tests/vga-text-guest");probe.key("ret")
        wait(probe,process,lambda:b"VGA_NATIVE_PROBE_PASS" in text_cells(probe),"native-probe")
        time.sleep(1)
        assert visible_text(probe,"tty2-ansi-probe") is not None
        print("TTY2 native probe passed",flush=True)
        tty2=text_cells(probe)
        result["checks"].append("ordinary login, 80x25 ioctl and real ANSI cursor query")
        for vt in range(3,7):
            login_vt(probe,process,vt)
            probe.text("/usr/lib/reliefos/tests/vga-text-guest");probe.key("ret")
            wait(probe,process,lambda:b"VGA_NATIVE_PROBE_PASS" in text_cells(probe),f"tty{vt}-probe")
            probe.frame(f"tty{vt}-state")
            print(f"TTY{vt} login and native probe passed",flush=True)
        for vt in range(3,7):
            select_vt(probe,process,vt)
            wait(probe,process,lambda v=vt:b"VGA_NATIVE_PROBE_PASS" in text_cells(probe) and
                f"/dev/tty{v}".encode() in text_cells(probe),f"tty{vt}-restored")
        select_vt(probe,process,2)
        wait(probe,process,lambda:text_cells(probe)==tty2,"tty2-restored")
        result["checks"].append("tty2-tty6 independent input/output state and exact tty2 cells restored")
        probe.key("ctrl-alt-f1")
        wait(probe,process,lambda:greeter(probe),"xdm-restored")
        probe.frame("03-xdm")
        desktop_login(probe,process)
        probe.frame("04-icewm")
        result["checks"].append("XDM ordinary login -> IceWM")
        probe.key("ctrl-alt-f2")
        wait(probe,process,lambda:b"VGA_NATIVE_PROBE_PASS" in text_cells(probe),"desktop-to-vga")
        probe.frame("desktop-to-vga")
        probe.key("ctrl-alt-f1")
        wait(probe,process,lambda:session_st_bounds(probe.frame("desktop-restored")),"icewm-restored")
        probe.frame("04-icewm")
        result["checks"].append("IceWM -> original VGA tty2 -> IceWM")
        result["passed"]=True
    finally:
        (args.output/"verification.json").write_text(json.dumps(result,ensure_ascii=False,indent=2)+"\n")
        probe.close()
    print(json.dumps(result,ensure_ascii=False,indent=2))


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qmp",type=Path,required=True)
    parser.add_argument("--serial",type=Path,required=True)
    parser.add_argument("--pid",type=int,required=True)
    parser.add_argument("--output",type=Path,required=True)
    parser.add_argument("--skip-boot",action="store_true",help="Resume an already booted, VGA-enabled QA VM")
    run(parser.parse_args())
