#!/usr/bin/env python3
"""Exercise installer input and window geometry on the shipped ISOs."""
import argparse
from pathlib import Path
import socket
import subprocess
import time

from qmp_terminal_smoke import text_keys
from run_gcc_probe_qemu import qmp_quit
from test_installer_responsiveness import InstallerProbe

ROOT = Path(__file__).resolve().parents[1]


class Probe(InstallerProbe):
    def __init__(self, path, output):
        self.output = output.resolve()
        self.socket = socket.socket(socket.AF_UNIX)
        self.socket.settimeout(10)
        self.socket.connect(str(path))
        self.stream = self.socket.makefile("rwb", buffering=0)
        self.stream.readline()
        self.command("qmp_capabilities")
        self._reset_installer_geometry()
        self.width, self.height = self.screenshot("boot").size

    def key(self, key):
        self.command("human-monitor-command", {"command-line": f"sendkey {key} 40"})
        time.sleep(0.10)

    def text(self, text):
        for key in text_keys(text):
            self.key(key)

    def click(self, x, y):
        self.move(x, y)
        self.button(True)
        time.sleep(0.1)
        self.button(False)

    def frame(self, name):
        frame = self.screenshot(name)
        self.width, self.height = frame.size
        return frame


def wait_log(serial, needle, process, timeout=90):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if serial.exists() and needle in serial.read_text(errors="replace"):
            return
        if process.poll() is not None:
            raise RuntimeError("QEMU exited unexpectedly")
        time.sleep(0.2)
    raise AssertionError(f"Missing serial output: {needle}")


def wait_for_installer_language(probe, process, timeout=90):
    deadline = time.monotonic() + timeout
    last_page = None
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("QEMU exited before the installer language page appeared")
        frame = probe.frame("installer-startup")
        last_page = probe.visible_page(frame)
        if last_page == 0:
            return frame
        time.sleep(0.5)
    raise AssertionError(
        f"Installer language page did not appear within {timeout}s; last visible page={last_page}")


def vga_arguments(profile):
    if profile == "vmware":
        return ["-vga", "vmware"]
    if profile == "std":
        return ["-device", "VGA,xres=1280,yres=720"]
    raise ValueError(f"Unsupported VGA profile: {profile}")


def installer(probe):
    frame = probe.frame("installer-language")
    assert probe.visible_page(frame) == 0, "Installer sidebar is cropped or scaled"
    # The FLTK sidebar must not be stretched across the content, and its
    # primary footer button must be visible at the detected window position.
    sidebar_sample = (probe.sidebar_left + 4, probe.sidebar_top + 400)
    content_sample = (probe.content_left + 4, probe.sidebar_top + 400)
    assert frame.getpixel(sidebar_sample) != frame.getpixel(content_sample), \
        "Sidebar covers content"
    for expected in (1, 2, 3):
        probe.click(*probe.footer_point())
        time.sleep(0.8)
        frame = probe.frame(f"installer-page-{expected}")
        assert probe.visible_page(frame) == expected, "Footer click did not change page"


def title_point(frame):
    # Ignore small active-color tab borders in other windows. Tests close each
    # maximized application before launching the next, so no selected list row
    # from a background maximized window can be confused with its title bar.
    for y in range(0, frame.height - 40, 4):
        xs = [x for x in range(0, frame.width, 4)
              if frame.getpixel((x, y)) == (0, 120, 212)]
        if len(xs) >= 100:
            return min(xs) + 120, y + 8
    raise AssertionError("Active title bar missing")


def desktop(probe):
    probe.text("nanotest")
    probe.key("ret")
    time.sleep(8)
    probe.text("nanotest")
    probe.key("ret")
    time.sleep(5)
    probe.key("meta_l")
    time.sleep(1.5)
    probe.text("terminal")
    probe.key("ret")
    time.sleep(15)
    frame = probe.frame("terminal-normal")
    # Detect the active title bar, independent of the initial window position.
    blue = lambda rgb: rgb == (0, 120, 212)
    title_x, title_y = title_point(frame)
    probe.click(title_x, title_y)
    probe.click(title_x, title_y)
    time.sleep(3)
    frame = probe.frame("terminal-maximized")
    assert blue(frame.getpixel((400, 15))) and blue(frame.getpixel((frame.width // 2, 15))), "Window did not maximize"
    for x, y in ((frame.width - 60, 150), (frame.width - 60, frame.height - 100),
                 (100, frame.height - 100)):
        assert max(frame.getpixel((x, y))) < 40, f"Maximized client blank at {(x, y)}"
    probe.text("vim -Nu NONE -n")
    probe.key("ret")
    time.sleep(6)
    probe.key("shift-semicolon")
    probe.text("echo &lines &columns")
    probe.key("ret")
    time.sleep(1)
    probe.frame("terminal-maximized-vim")
    probe.key("shift-semicolon")
    probe.text("q")
    probe.key("ret")
    time.sleep(1)
    probe.click(probe.width - 37, 15)
    time.sleep(2)
    frame = probe.frame("terminal-restored")
    assert not blue(frame.getpixel((400, 15))) and blue(frame.getpixel((title_x, title_y))), "Window did not restore"
    for app in ("fileman", "taskmgr"):
        probe.key("meta_l")
        time.sleep(1)
        probe.text(app)
        probe.key("ret")
        time.sleep(8)
        frame = probe.frame(f"{app}-normal")
        title_x, title_y = title_point(frame)
        probe.click(title_x, title_y)
        probe.click(title_x, title_y)
        time.sleep(3)
        frame = probe.frame(f"{app}-maximized")
        assert blue(frame.getpixel((400, 15))) and blue(frame.getpixel((frame.width // 2, 15))), f"{app} did not maximize"
        # The application status area must reach the enlarged client's bottom.
        assert min(frame.getpixel((frame.width - 80, frame.height - 50))) < 250, f"{app} status area missing"
        probe.click(probe.width - 15, 15)
        time.sleep(2)
        assert not blue(probe.frame(f"{app}-closed").getpixel((400, 15))), f"{app} did not close"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("case", choices=("tty", "installer", "desktop", "installed"))
    parser.add_argument("--output", type=Path, default=ROOT / "build/ui-regressions")
    parser.add_argument("--iso", type=Path)
    parser.add_argument("--vga", choices=("std", "vmware"), default="std",
                        help="QEMU display device (std preserves the legacy 1280x720 VGA device)")
    parser.add_argument("--install", action="store_true", help="Install onto this test's disposable scratch disk")
    parser.add_argument("--disk", type=Path, help="Previously installed test disk to boot without the ISO")
    args = parser.parse_args()
    if args.case == "installed" and (not args.disk or not args.disk.is_file()):
        parser.error("installed requires an existing --disk")
    out = args.output / args.case
    out.mkdir(parents=True, exist_ok=True)
    serial, qmp = out / "serial.log", out / "qmp.sock"
    qmp.unlink(missing_ok=True)
    iso = ROOT / "build/images" / ("reliefos-live.iso" if args.case == "desktop" else "reliefos-installer.iso")
    if args.iso:
        iso = args.iso.resolve()
    disk = out / "scratch.raw"
    if args.case in ("tty", "installer"):
        with disk.open("xb") as file:
            file.truncate(2 * 1024**3)
    command = ["qemu-system-x86_64", "-enable-kvm", "-cpu", "host", "-machine", "q35",
               "-m", "4096", "-smp", "2", "-bios", "/usr/share/edk2/x64/OVMF.4m.fd",
               "-display", "none", "-serial", f"file:{serial}",
               *vga_arguments(args.vga), "-device", "qemu-xhci", "-device", "usb-tablet",
               "-netdev", "user,id=net0", "-device", "e1000,netdev=net0",
               "-qmp", f"unix:{qmp},server=on,wait=off",
               "-no-reboot", "-no-shutdown"]
    if args.case == "installed":
        command += ["-drive", f"file={args.disk.resolve()},format=raw,if=ide", "-boot", "c"]
    else:
        command += ["-cdrom", str(iso), "-boot", "d"]
    if args.case in ("tty", "installer"):
        command += ["-drive", f"file={disk},format=raw,if=ide"]
    with (out / "qemu.log").open("w") as errors:
        process = subprocess.Popen(command, stdout=errors, stderr=errors, cwd=ROOT)
        probe = None
        try:
            deadline = time.monotonic() + 15
            while not qmp.exists() and time.monotonic() < deadline:
                time.sleep(0.1)
            probe = Probe(qmp, out)
            if args.case == "tty":
                time.sleep(4)
                probe.frame("grub")
                probe.key("down")
                probe.key("ret")
                wait_log(serial, "Mode [install/update]:", process)
                probe.text("install")
                probe.key("ret")
                time.sleep(3)
                probe.frame("tty-after-install")
                wait_log(serial, "Select disk number", process, 10)
                probe.text("0")
                probe.key("ret")
                wait_log(serial, "Type INSTALL", process, 10)
                probe.text("INSTALL" if args.install else "no")
                probe.key("ret")
                if args.install:
                    wait_log(serial, "Reboot now?", process, 900)
                    probe.frame("tty-installed")
                    probe.text("n")
                    probe.key("ret")
                    wait_log(serial, "Installation finished.", process, 10)
                else:
                    wait_log(serial, "Installation not confirmed", process, 10)
            elif args.case == "installer":
                wait_log(serial, "path=/usr/lib/reliefos/apps/installer/installer.elf", process)
                wait_for_installer_language(probe, process, timeout=90)
                installer(probe)
            else:
                wait_log(serial, "[oobe.elf] starting first-run", process)
                time.sleep(8)
                desktop(probe)
            assert "KERNEL PANIC" not in serial.read_text(errors="replace")
            print(f"PASS {args.case}: {out}", flush=True)
        finally:
            if probe is not None:
                probe.close()
            if process.poll() is None:
                qmp_quit(qmp, process)
            qmp.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
