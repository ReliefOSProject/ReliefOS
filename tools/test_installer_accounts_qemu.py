#!/usr/bin/env python3
"""Install onto an exclusive scratch disk, then verify actual login credentials."""
import argparse
from collections import Counter
from contextlib import contextmanager
from pathlib import Path
import subprocess
import re
import json
import time

from test_installer_window_qemu import Probe as WindowProbe, wait_log
from qmp_terminal_smoke import text_keys
from run_gcc_probe_qemu import qmp_quit

ROOT = Path(__file__).resolve().parents[1]
USER_PASSWORD = "U!" + "u" * 30
ROOT_PASSWORD = "r"
XORG_EXCLUDED_APPS = (
    "calc", "doom", "doomlauncher", "fileman", "imageview", "installer",
    "leonmmcoset", "minesweeper", "osver", "paint", "taskmgr", "xiaobai",
    "glxgears", "portablegl",
)
XORG_COMMANDS = ("Xorg", "icewm", "xdm")


def xorg_apk_packages():
    lock = json.loads((ROOT / "configs/dependencies.lock.json").read_text())
    return {
        dependency["id"][len("alpine-"):]
        for dependency in lock["dependencies"]
        if dependency.get("kind") == "apk"
        and dependency.get("feature") == "xorg"
        and dependency.get("id", "").startswith("alpine-")
    }


class Probe(WindowProbe):
    def enable_installer_geometry(self):
        """Compatibility no-op: the shared FLTK detector is always active."""
        return None

    def enable_motif_geometry(self):
        """Compatibility alias for callers written against the Motif probe."""
        self.enable_installer_geometry()

    def clear_installer_geometry(self):
        self._reset_installer_geometry()

    def clear_motif_geometry(self):
        """Compatibility alias for callers written against the Motif probe."""
        self.clear_installer_geometry()

    def click(self, x, y):
        super().click(x, y)
        # QMP acknowledges enqueueing, not the guest focus change.
        time.sleep(0.5)

    def key(self, key):
        super().key(key)
        # Allow guest key release processing before repeating the same key.
        time.sleep(0.15)

    def text(self, text):
        punctuation = {";": "semicolon", "(": "shift-9", ")": "shift-0",
                       ">": "shift-dot", "$": "shift-4", "?": "shift-slash",
                       "'": "apostrophe", '"': "shift-apostrophe", "!": "shift-1"}
        for character in text:
            self.key(punctuation.get(character, text_keys(character)[0]))
        # The keyboard and USB pointer travel through separate guest queues.
        time.sleep(1)


@contextmanager
def boot(output, disk, iso=None, *, smp=2):
    output.mkdir(parents=True, exist_ok=True)
    serial, qmp = output / "serial.log", output / "qmp.sock"
    command = ["qemu-system-x86_64", "-enable-kvm", "-cpu", "host", "-machine", "q35",
               "-m", "4096", "-smp", str(smp), "-bios", "/usr/share/edk2/x64/OVMF.4m.fd",
               "-display", "none", "-serial", f"file:{serial}",
               "-vga", "vmware", "-device", "qemu-xhci", "-device", "usb-tablet",
               "-netdev", "user,id=net0", "-device", "e1000,netdev=net0",
               "-drive", f"file={disk},format=raw,if=ide",
               "-qmp", f"unix:{qmp},server=on,wait=off", "-no-reboot", "-no-shutdown"]
    # An installed disk has its own EFI loader. Give the optical device an
    # explicit firmware priority so update tests actually boot the installer.
    command += (["-drive", f"file={iso},format=raw,media=cdrom,if=none,id=installer-cd",
                 "-device", "ide-cd,drive=installer-cd,bootindex=1", "-boot", "d"]
                if iso else ["-boot", "c"])
    with (output / "qemu.log").open("w") as errors:
        process = subprocess.Popen(command, stdout=errors, stderr=errors, cwd=ROOT)
        probe = None
        try:
            deadline = time.monotonic() + 15
            while not qmp.exists() and time.monotonic() < deadline:
                if process.poll() is not None: raise RuntimeError("QEMU failed to start")
                time.sleep(.1)
            probe = Probe(qmp, output)
            probe.disk = disk
            yield probe, serial, process
        finally:
            try:
                if probe is not None:
                    try: probe.frame("last-frame")
                    finally: probe.close()
            finally:
                if process.poll() is None: qmp_quit(qmp, process)
                qmp.unlink(missing_ok=True)


def next_page(probe, expected_page=None):
    if expected_page is None:
        enable_geometry = getattr(probe, "enable_installer_geometry", None)
        if enable_geometry is None:
            enable_geometry = getattr(probe, "enable_motif_geometry", None)
        if enable_geometry is not None:
            enable_geometry()
    before = probe.visible_page(probe.frame("before-next"))
    if before is None:
        raise AssertionError("Installer page is not visible before clicking Next")
    probe.click(*probe.footer_point())
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        frame = probe.frame(f"after-next-{expected_page}")
        after = probe.visible_page(frame)
        if expected_page is not None and after == expected_page:
            return
        if after is not None and after != before and expected_page is None:
            return
        if after is not None and after != before and expected_page is not None:
            raise AssertionError(f"Next changed installer page {before} to {after}, expected {expected_page}")
        time.sleep(.1)
    if expected_page is None:
        raise AssertionError(f"Next left the installer on page {before}")
    raise AssertionError(f"Next left the installer on page {before}; expected {expected_page}")


def previous_page(probe, expected_page=None):
    if expected_page is None:
        enable_geometry = getattr(probe, "enable_installer_geometry", None)
        if enable_geometry is None:
            enable_geometry = getattr(probe, "enable_motif_geometry", None)
        if enable_geometry is not None:
            enable_geometry()
    before = probe.visible_page(probe.frame("before-previous"))
    if before is None:
        raise AssertionError("Installer page is not visible before clicking Previous")
    probe.click(*probe.footer_point(previous=True))
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        frame = probe.frame(f"after-previous-{expected_page}")
        after = probe.visible_page(frame)
        if expected_page is not None and after == expected_page:
            return
        if after is not None and after != before and expected_page is None:
            return
        if after is not None and after != before and expected_page is not None:
            raise AssertionError(f"Previous changed installer page {before} to {after}, expected {expected_page}")
        time.sleep(.1)
    if expected_page is None:
        raise AssertionError(f"Previous left the installer on page {before}")
    raise AssertionError(f"Previous left the installer on page {before}; expected {expected_page}")


def wait_installer_sidebar(probe, process, frame_name, timeout=60):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        frame = probe.frame(frame_name)
        if probe.visible_page(frame) is not None:
            return frame
        if process.poll() is not None:
            raise RuntimeError("QEMU exited before the installer FLTK window appeared")
        time.sleep(.5)
    raise AssertionError("Installer FLTK sidebar did not appear within 60 seconds")


def wait_motif_sidebar(probe, process, frame_name, timeout=60):
    """Compatibility alias for the former Motif-specific helper name."""
    return wait_installer_sidebar(probe, process, frame_name, timeout)


def installer_root_extent(text):
    """Locate the classified reserved filesystem carrier, excluding newc bytes."""
    matches = re.findall(r"\[boot\] module=[0-9]+ name=(?:reliefos|leonos)-installer-root start=(0x[0-9a-fA-F]+) bytes=([0-9]+)", text)
    if len(matches) != 1:
        raise AssertionError("Missing or ambiguous installer filesystem boot module")
    start, size = int(matches[0][0], 16), int(matches[0][1])
    if not start or size < 512 or size % 512 or start + size > 1 << 64:
        raise AssertionError("Invalid installer filesystem boot module extent")
    return start, size


def installer_log(probe, serial):
    """Read a diagnostic snapshot of the write-through reserved RAM filesystem.

    Success admission separately reads the live guest log through its rescue
    TTY; a possibly incomplete cached snapshot never proves installation.
    """
    start, size = installer_root_extent(serial.read_text(errors="replace"))
    memory = probe.output / "installer-root.ext4"
    log_path = probe.output / "installer.log"
    probe.command("stop")
    try:
        probe.command("pmemsave", {"val": start, "size": size,
                                  "filename": str(memory)})
    finally:
        probe.command("cont")
    subprocess.run(["debugfs", "-R", f"dump /var/log/installer-session.log {log_path}",
                    str(memory)], check=True)
    return log_path.read_text()


def debugfs_output(image, command):
    return subprocess.run(["debugfs", "-R", command, str(image)], check=True,
                          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                          text=True).stdout


def debugfs_directory_names(image, path):
    listing = debugfs_output(image, f"ls -l {path}")
    return {line.split()[-1] for line in listing.splitlines()
            if line.split() and not line.startswith("debugfs:")}


def assert_desktop_profile(image, skip_xorg):
    backend = debugfs_output(image, "cat /etc/reliefos/desktop-backend").strip()
    expected = "console" if skip_xorg else "xorg"
    assert backend == expected, f"Expected desktop backend {expected!r}, got {backend!r}"

    installed = debugfs_output(image, "cat /lib/apk/db/installed")
    package_names = {line[2:] for line in installed.splitlines() if line.startswith("P:")}
    xorg_packages = xorg_apk_packages()
    assert xorg_packages, "dependency lock contains no Xorg APK packages"
    if skip_xorg:
        present = sorted(xorg_packages & package_names)
        assert not present, f"Console image includes Xorg APK records: {present}"
    else:
        required = {"xorg-server", "icewm", "xdm"}
        assert required <= package_names, f"Xorg APK records missing: {sorted(required - package_names)}"

    binaries = debugfs_directory_names(image, "/usr/bin")
    apps = debugfs_directory_names(image, "/usr/lib/reliefos/apps")
    if skip_xorg:
        unexpected = sorted((set(XORG_COMMANDS) | set(XORG_EXCLUDED_APPS)) & binaries)
        unexpected_apps = sorted(set(XORG_EXCLUDED_APPS) & apps)
        assert not unexpected, f"Console image includes Xorg commands: {unexpected}"
        assert not unexpected_apps, f"Console image includes Xorg applications: {unexpected_apps}"


def installer_finish_log(probe, serial, process):
    """Read current redirected output through the real installer rescue terminal."""
    start = serial.stat().st_size
    probe.key("ctrl-alt-f2")
    time.sleep(1)
    probe.text("cat /var/log/installer-session.log > /dev/ttyS0; echo INSTALLER_LOG_READ_DONE > /dev/ttyS0")
    probe.key("ret")
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        assert process.poll() is None, "QEMU exited before reading installer results"
        with serial.open("r", errors="replace") as stream:
            stream.seek(start)
            text = stream.read()
        if "INSTALLER_LOG_READ_DONE" in text:
            probe.key("ctrl-alt-f1")
            time.sleep(1)
            (probe.output / "installer-finish.log").write_text(text)
            return text
        time.sleep(.2)
    raise AssertionError("Installer rescue terminal did not return its real log")


def wait_install(probe, serial, process, *, tty=False, timeout=2400):
    """Require the real completion marker and finish UI; cached RAM logs are diagnostic."""
    if timeout <= 0:
        raise ValueError("installation timeout must be positive")
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        log = serial.read_text(errors="replace")
        if any("[installer.elf]" in line and " ret=-" in line for line in log.splitlines()):
            raise AssertionError("Installer failed; see serial.log and last-frame.png")
        if process.poll() is not None:
            raise RuntimeError("QEMU exited during installation")
        completed = "[installer.elf] installation completed successfully" in log
        frame = probe.frame("tty-install-progress" if tty else "progress")
        finished = "Reboot now?" in log if tty else probe.visible_page(frame) == 9
        if finished and not tty:
            current = installer_finish_log(probe, serial, process)
            assert "[installer.elf] installation completed successfully" in current, current[-10000:]
            probe.frame("installed")
            return
        if completed and finished:
            probe.frame("installed")
            return
        time.sleep(5)
    raise AssertionError("Installation timed out")


def wait_desktop(probe, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        frame = probe.frame("desktop")
        if frame.getpixel((probe.width // 2, probe.height - 10)) != (0, 120, 212):
            return frame
        time.sleep(.5)
    raise AssertionError("Login did not transition from the ReliefOS login screen to the desktop")


def xorg_checkbox_crop(probe, name):
    x, y = probe.xorg_indicator_point()
    probe.move(probe.width - 10, 10)
    time.sleep(.1)
    frame = probe.frame(name)
    return frame.crop((x - 9, y - 9, x + 10, y + 10))


def xorg_checkbox_checked(indicator):
    # FLTK's gtk+ scheme draws a contrasting check mark inside the square.
    # Ignore the bevel and detect pixels that differ from the uniform interior,
    # independent of whether the selected scheme chooses a light or dark mark.
    center = indicator.crop((5, 5, 14, 14))
    colors = [center.getpixel((x, y)) for y in range(center.height)
              for x in range(center.width)]
    background, _ = Counter(colors).most_common(1)[0]
    return sum(1 for color in colors
               if max(abs(channel - background[index])
                      for index, channel in enumerate(color)) > 40) >= 3


def install_gui(probe, serial, process, *, skip_xorg=False, chinese=False, install_timeout=2400):
    """Complete the real GUI, optionally selecting Chinese before account setup."""
    probe.enable_installer_geometry()
    wait_log(serial, "path=/usr/lib/reliefos/apps/installer/installer.elf", process)
    frame = wait_installer_sidebar(probe, process, "language")
    assert probe.visible_page(frame) == 0, "Installer did not start at the language page"
    if probe.sidebar_bottom - probe.sidebar_top < 680:
        probe.key("alt-f10")
        time.sleep(.8)
        probe.clear_installer_geometry()
        frame = wait_installer_sidebar(probe, process, "language-maximized")
        assert probe.visible_page(frame) == 0, "Installer left the language page while maximizing"
    if probe.sidebar_bottom - probe.sidebar_top < 680:
        raise AssertionError("Installer FLTK footer remains clipped after maximizing")
    next_x, next_y = probe.footer_point()
    if not (0 <= next_x < probe.width and 0 <= next_y < probe.height):
        raise AssertionError("Installer FLTK Next button is outside the visible screen")
    if chinese:
        probe.click(probe.content_left + 30, probe.sidebar_top + 191)
        probe.frame("language-chinese")
    for expected_page in range(1, 7):
        next_page(probe, expected_page)
    for index, text in enumerate(("alice", USER_PASSWORD, USER_PASSWORD, ROOT_PASSWORD, ROOT_PASSWORD)):
        probe.click(*probe.account_field_point(index))
        probe.text(text)
    probe.frame("accounts")
    next_page(probe, 7)
    probe.frame("confirmation")

    # Exercise the Xorg choice across a real page rebuild. The screenshot of
    # the initial state is also retained for checking the default selection.
    checkbox_x, checkbox_y = probe.xorg_indicator_point()
    confirm_x, confirm_y = probe.confirm_field_point()
    probe.click(confirm_x, confirm_y)
    probe.text("INSTALL")
    default_xorg = xorg_checkbox_crop(probe, "confirmation-xorg-default")
    assert xorg_checkbox_checked(default_xorg), "Xorg option is not checked by default"
    probe.click(checkbox_x, checkbox_y)
    probe.click(confirm_x, confirm_y)
    changed_xorg = xorg_checkbox_crop(probe, "confirmation-xorg-toggled")
    assert not xorg_checkbox_checked(changed_xorg), "Xorg checkbox did not clear when clicked"
    previous_page(probe, 6)
    probe.frame("accounts-xorg-back")
    next_page(probe, 7)
    probe.click(confirm_x, confirm_y)
    retained_xorg = xorg_checkbox_crop(probe, "confirmation-xorg-retained")
    assert not xorg_checkbox_checked(retained_xorg), "Xorg choice was lost when returning to Confirm"
    assert retained_xorg.tobytes() == changed_xorg.tobytes(), "Xorg indicator changed after returning to Confirm"
    if not skip_xorg:
        probe.click(checkbox_x, checkbox_y)
        probe.click(confirm_x, confirm_y)
        enabled_xorg = xorg_checkbox_crop(probe, "confirmation-xorg-enabled")
        assert xorg_checkbox_checked(enabled_xorg), "Xorg checkbox did not return to enabled state"
        assert enabled_xorg.tobytes() == default_xorg.tobytes(), "Xorg indicator did not return to its initial state"

    # Returning from Accounts rebuilds Confirm and clears its validation field.
    probe.click(confirm_x, confirm_y)
    probe.text("INSTALL")
    probe.click(confirm_x, confirm_y)
    probe.frame("confirmation-ready")
    next_page(probe, 8)
    wait_install(probe, serial, process, timeout=install_timeout)


def install_tty(probe, serial, process, components="none", *, skip_xorg=False,
                install_bios=True, install_uefi=True, select_entry=True, install_timeout=2400):
    """Answer the real TTY installer, including both independent loader choices."""
    if select_entry:
        from test_vt_qemu import ocr
        # Stop the five-second menu timer before OCR can consume that window.
        # Up preserves entry zero and is harmless during firmware startup.
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            probe.key("up")
            if "install reliefos" in ocr(probe.frame("grub-console-entry")).lower().replace("0", "o"):
                break
        else:
            raise AssertionError("Installer GRUB menu did not appear")
        probe.key("down")
        probe.key("ret")
        wait_log(serial, "path=/bin/bash", process)
        # Preserve the real keyboard TTY as stdin; tee mirrors visible prompts
        # to serial for exact, bounded assertions without OCR transcription.
        probe.text("installer 2>/var/log/installer-session.log | tee /dev/ttyS0")
        probe.key("ret")
    answers = (("Mode [install/update]: ", "install"),
               ("Select disk number (r to refresh, q to quit): ", "0"),
               ("partition> ", "auto"), ("partition> ", "done"),
               ("Username: ", "alice"), ("Password: ", USER_PASSWORD),
               ("Confirm password: ", USER_PASSWORD), ("root password: ", ROOT_PASSWORD),
               ("Confirm root password: ", ROOT_PASSWORD),
               ("Install Xorg desktop environment? [Y/n]: ", "n" if skip_xorg else ""),
               ("Install or update BIOS bootloader? [Y/n]: ", "" if install_bios else "n"),
               ("Install or update UEFI bootloader? [Y/n]: ", "" if install_uefi else "n"),
               ("Type INSTALL to confirm the partition layout and installation: ", "INSTALL"))
    for prompt, answer in answers:
        wait_log(serial, prompt, process, timeout=120)
        probe.text(answer)
        probe.key("ret")
    wait_install(probe, serial, process, tty=True, timeout=install_timeout)


def login_tty(probe, serial, process, root, *, skip_xorg=False):
    # Console-profile installs expose tty1 directly. Xorg-profile installs
    # keep a text getty on tty2, even while a graphical greeter owns tty1.
    from test_vt_qemu import wait_text
    username = "root" if root else "alice"
    if skip_xorg:
        wait_text(probe, process, "console-tty1-login", "login:", timeout=90)
    else:
        # Xorg takes tty1 after init completes. Switch only after that handoff
        # so its startup cannot steal the test's text login back from tty2.
        wait_log(serial, "VT graphics vt=1 enabled=1", process, timeout=120)
        probe.key("ctrl-alt-f2")
        wait_text(probe, process, "tty-login", "login:", timeout=90)
    probe.text(username)
    probe.key("ret")
    wait_text(probe, process, "tty-password", "Password:")
    probe.text(ROOT_PASSWORD if root else USER_PASSWORD)
    probe.key("ret")
    time.sleep(1)
    login_ready = "LOGIN_READY_ROOT" if root else "LOGIN_READY_USER"
    probe.text(f"echo {login_ready}")
    probe.key("ret")
    wait_text(probe, process, "tty-login-ready", login_ready, timeout=60)
    if root:
        command = ("cat /proc/self/status; echo root-ok > /root/installer-root-check; "
                   "cat /root/installer-root-check; echo group-only > /tmp/root-group-only; "
                   "chmod 040 /tmp/root-group-only; echo ROOT_SESSION_DONE")
    else:
        command = (
            "cat /proc/self/status; echo HOME=$HOME; echo user-ok > $HOME/installer-user-check; "
            "cat $HOME/installer-user-check; "
            "if (echo bad > /root/installer-denied) 2>/dev/null; then echo ACCESS_FAILED; else echo ROOT_DENIED; fi; "
            "if (echo bad > /etc/passwd) 2>/dev/null; then echo ACCESS_FAILED; else echo PASSWD_DENIED; fi; "
            "if cat /tmp/root-group-only 2>/dev/null; then echo ACCESS_FAILED; else echo ROOT_GROUP_DENIED; fi; "
            "test -e /usr/bin/python3; echo PYTHON_STATUS=$?; "
            "test -e /usr/bin/gcc; echo GCC_STATUS=$?; "
            "test -e /usr/bin/tcc; echo TCC_STATUS=$?; echo USER_SESSION_DONE")
    evidence_path = "/root/installer-root-evidence" if root else "/home/alice/installer-user-evidence"
    marker = "ROOT_SESSION_DONE" if root else "USER_SESSION_DONE"
    probe.text("( " + command + " ) > " + evidence_path + " 2>&1; sync")
    probe.key("ret")
    # Read back only after the shell has returned and flushed its writes. The
    # ordinary user need not gain access to the root-owned serial device.
    time.sleep(3)
    probe.text("cat " + evidence_path)
    probe.key("ret")
    wait_text(probe, process, "session-evidence", marker, timeout=60)
    table = json.loads(subprocess.check_output(["sfdisk", "--json", str(probe.disk)], text=True))["partitiontable"]
    partition = next(part for part in table["partitions"]
                     if part["type"].lower() == "0fc63daf-8483-4772-8e79-3d69d8477de4")
    image = probe.output / "evidence-root.ext2"
    probe.command("stop")
    try:
        subprocess.run(["dd", f"if={probe.disk}", f"of={image}", "bs=4M", "iflag=skip_bytes,count_bytes",
                        f"skip={partition['start'] * table['sectorsize']}",
                        f"count={partition['size'] * table['sectorsize']}", "conv=sparse", "status=none"], check=True)
    finally:
        probe.command("cont")
    log = subprocess.check_output(["debugfs", "-R", f"cat {evidence_path}", str(image)],
                                  stderr=subprocess.DEVNULL, text=True).replace("\r", "")
    (probe.output / "session-evidence.txt").write_text(log)
    assert marker in log.splitlines(), log
    assert_desktop_profile(image, skip_xorg)
    expected = ["0" if root else "1000"] * 4
    for field in ("Uid:", "Gid:"):
        identities = [line.split()[1:] for line in log.splitlines() if line.startswith(field)]
        assert identities and all(identity == expected for identity in identities), (field, identities)
    if not root:
        for line in ("ROOT_DENIED", "PASSWD_DENIED", "ROOT_GROUP_DENIED", "HOME=/home/alice", "user-ok"):
            assert "\n" + line + "\n" in log, line
        assert "\nACCESS_FAILED\n" not in log
        for tool in ("PYTHON", "GCC", "TCC"):
            assert f"\n{tool}_STATUS=1\n" in log
    else:
        assert "\nroot-ok\n" in log
    probe.frame(("console-tty1" if skip_xorg else "tty2") + ("-root-session" if root else "-user-session"))


def login_desktop(probe, serial, process):
    wait_log(serial, "path=/usr/lib/reliefos/apps/login/login.elf", process)
    time.sleep(5)
    probe.frame("login")
    probe.key("down")
    probe.text(USER_PASSWORD)
    probe.key("ret")
    wait_desktop(probe)
    time.sleep(5)
    probe.key("meta_l")
    time.sleep(1)
    probe.text("terminal")
    probe.key("ret")
    wait_log(serial, "path=/usr/lib/reliefos/apps/terminal/terminal.elf", process)
    probe.text("fastfetch --format json --structure OS:Kernel")
    probe.key("ret")
    time.sleep(3)
    probe.frame("terminal-fastfetch")
    time.sleep(2)
    probe.text("cat /proc/self/status; echo HOME=$HOME")
    probe.key("ret")
    time.sleep(3)
    probe.frame("terminal-identity")
    log = serial.read_text(errors="replace")
    assert "name=terminal.elf code=127" not in log, "Terminal shell failed to execute"
    assert "name=terminal.elf code=126" not in log, "Terminal shell failed to drop credentials"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", type=Path, default=ROOT / "build/images/reliefos-installer.iso")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--installer", choices=("gui", "tty"), default="gui")
    parser.add_argument("--without-xorg", action="store_true",
                        help="Skip the optional Xorg desktop during a fresh install")
    parser.add_argument("--login-only", action="store_true", help="Boot this test's existing scratch disk without reinstalling")
    parser.add_argument("--desktop", action="store_true", help="Also log into the installed desktop and capture Terminal identity")
    parser.add_argument("--desktop-only", action="store_true", help="Only boot the existing test disk into the desktop")
    args = parser.parse_args()
    if args.without_xorg and (args.desktop or args.desktop_only):
        parser.error("--desktop and --desktop-only require Xorg")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    disk = output / "scratch.raw"
    if args.desktop_only:
        if not disk.is_file(): parser.error("--desktop-only requires an existing scratch disk")
        with boot(output / "desktop", disk) as session:
            login_desktop(*session)
        return
    if args.login_only:
        if not disk.is_file(): parser.error("--login-only requires an existing scratch disk")
    else:
        with disk.open("xb") as file: file.truncate(8 * 1024**3)
        with boot(output / "install", disk, args.iso.resolve()) as session:
            if args.installer == "gui":
                install_gui(*session, skip_xorg=args.without_xorg)
            else:
                install_tty(*session, skip_xorg=args.without_xorg)
    with boot(output / "root", disk) as session:
        login_tty(*session, True, skip_xorg=args.without_xorg)
    with boot(output / "user", disk) as session:
        login_tty(*session, False, skip_xorg=args.without_xorg)
    if args.desktop:
        with boot(output / "desktop", disk) as session:
            login_desktop(*session)
    print(f"PASS: installer, ordinary/root credentials: {output}")


if __name__ == "__main__":
    main()
