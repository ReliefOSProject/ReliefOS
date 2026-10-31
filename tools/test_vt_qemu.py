#!/usr/bin/env python3
"""Boot the installed image and exercise graphical/text VT switching in QEMU."""
from pathlib import Path
import subprocess
import tempfile
import time
import shutil
import os

from test_installer_window_qemu import wait_log
from test_installer_accounts_qemu import Probe
from run_gcc_probe_qemu import qmp_quit

ROOT = Path(__file__).resolve().parents[1]
IMAGE = Path(os.environ.get("VT_IMAGE", ROOT / "out/x86_64/release/images/reliefos.raw"))
OUTPUT = Path(os.environ.get("VT_OUTPUT", ROOT / "build/vt-qemu"))
TEXT_ONLY = os.environ.get("VT_TEXT_ONLY") == "1"


def ocr(frame, psm=6):
    with tempfile.NamedTemporaryFile(suffix=".png") as source:
        frame.resize((frame.width * 2, frame.height * 2)).save(source.name)
        return subprocess.run(["tesseract", source.name, "stdout", "--psm", str(psm)],
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                              text=True, check=True).stdout


def wait_graphical(probe, process, name, timeout=30):
    """A text-only/blank VT must never count as a ready graphical session."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        assert process.poll() is None, "QEMU exited before the graphical session"
        frame = probe.frame(name)
        sample = frame.resize((160, 90))
        pixels = [sample.getpixel((x, y)) for y in range(90) for x in range(160)]
        if sum(max(pixel) - min(pixel) > 25 for pixel in pixels) > len(pixels) // 10:
            return frame
        time.sleep(.5)
    raise AssertionError(f"Graphical VT never became visible: {name}")


def wait_text(probe, process, name, needle, timeout=30, psm=6):
    deadline = time.monotonic() + timeout
    visible = ""
    while time.monotonic() < deadline:
        assert process.poll() is None, "QEMU exited while waiting for terminal output"
        visible = ocr(probe.frame(name), psm)
        if needle.lower() in visible.lower():
            return visible
        time.sleep(.5)
    raise AssertionError(f"Missing {needle!r} on {name}: {visible!r}")


def login_root(probe, process):
    probe.text("root")
    probe.key("ret")
    wait_text(probe, process, "tty2-password", "Password:")
    probe.text("root")
    probe.key("ret")
    wait_text(probe, process, "tty2-shell", "built-in shell")


def main():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    serial = OUTPUT / "serial.log"
    serial.write_text("")
    with tempfile.TemporaryDirectory(prefix="reliefos-vt-qmp-") as directory:
        qmp = Path(directory) / "qmp.sock"
        vars_path = Path(directory) / "OVMF_VARS.fd"
        shutil.copyfile("/usr/share/edk2/x64/OVMF_VARS.4m.fd", vars_path)
        command = ["qemu-system-x86_64", "-enable-kvm", "-cpu", "host", "-machine", "q35",
                   "-m", "4096", "-smp", "2",
                   "-drive", "if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd",
                   "-drive", f"if=pflash,format=raw,file={vars_path}",
                   "-display", "none", "-serial", f"file:{serial}",
                   "-device", "VGA,xres=1280,yres=720",
                   "-netdev", "user,id=n", "-device", "e1000,netdev=n",
                   "-drive", f"file={IMAGE},if=ide,format=raw,snapshot={os.environ.get('VT_SNAPSHOT', 'on')}",
                   "-qmp", f"unix:{qmp},server=on,wait=off", "-no-reboot", "-no-shutdown"]
        with (OUTPUT / "qemu.log").open("w") as errors:
            process = subprocess.Popen(command, stdout=errors, stderr=errors, cwd=ROOT)
            probe = None
            try:
                deadline = time.monotonic() + 15
                while not qmp.exists() and time.monotonic() < deadline:
                    if process.poll() is not None:
                        raise RuntimeError("QEMU exited before QMP became ready")
                    time.sleep(.1)
                probe = Probe(qmp, OUTPUT)
                time.sleep(5)
                print("early:", repr(ocr(probe.frame("early"))[:300]), flush=True)
                wait_log(serial, "boot complete:", process, timeout=100)
                if TEXT_ONLY:
                    wait_text(probe, process, "tty1-text", "login:")
                else:
                    wait_graphical(probe, process, "tty1-desktop")
                    wait_text(probe, process, "tty1-login-ui", "root")
                for number in range(2, 7):
                    probe.key(f"ctrl-alt-f{number}")
                    time.sleep(1.5)
                    visible = ocr(probe.frame(f"tty{number}"))
                    assert "login:" in visible, f"tty{number} has no login prompt: {visible!r}"
                    print(f"tty{number}:", repr(visible[:240]), flush=True)
                probe.key("ctrl-alt-f1")
                if TEXT_ONLY:
                    wait_text(probe, process, "tty1-restored", "login:")
                else:
                    wait_graphical(probe, process, "tty1-restored")
                    wait_text(probe, process, "tty1-login-restored", "root")
                probe.key("ctrl-alt-f2")
                wait_text(probe, process, "tty2-login", "login:")
                login_root(probe, process)
                probe.text("test -t 0 && test -t 1 && test -t 2 && echo VT2-STDIO-OK >/dev/ttyS0")
                probe.key("ret")
                wait_log(serial, "VT2-STDIO-OK", process, timeout=20)
                probe.text("echo VT2-TTY=$(tty) >/dev/ttyS0")
                probe.key("ret")
                wait_log(serial, "VT2-TTY=/dev/tty2", process, timeout=20)
                probe.frame("tty2-logged-in")
                if os.environ.get("VT_PROBE") == "1":
                    probe.text("/tmp/vt-probe")
                    probe.key("ret")
                    wait_log(serial, "VT-PROBE-PASS", process, timeout=30)

                    probe.key("ctrl-alt-f2")
                before = serial.read_text().count("path=/bin/login ")
                probe.text("exit")
                probe.key("ret")
                deadline = time.monotonic() + 20
                while serial.read_text().count("path=/bin/login ") <= before:
                    assert process.poll() is None and time.monotonic() < deadline, "getty did not respawn"
                    time.sleep(.2)
                wait_text(probe, process, "tty2-logout", "login:")
                print("tty2 login, terminal stdio and logout passed", flush=True)
            finally:
                try:
                    if probe:
                        try:
                            probe.frame("last-frame")
                        finally:
                            probe.close()
                finally:
                    if process.poll() is None:
                        qmp_quit(qmp, process)


if __name__ == "__main__":
    main()
