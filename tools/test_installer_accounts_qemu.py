#!/usr/bin/env python3
"""Install onto an exclusive scratch disk, then verify actual login credentials."""
import argparse
from difflib import SequenceMatcher
from collections import Counter
from contextlib import contextmanager
from pathlib import Path
import subprocess
import re
import json
import time
import uuid

from test_installer_window_qemu import (
    Probe as WindowProbe, firmware_arguments, qemu_acceleration, wait_log,
)
from qmp_terminal_smoke import text_keys
from run_gcc_probe_qemu import qmp_quit

ROOT = Path(__file__).resolve().parents[1]
USER_PASSWORD = "U!" + "u" * 30
ROOT_PASSWORD = "r"
XORG_EXCLUDED_APPS = (
    "calc", "doom", "doomlauncher", "fileman", "imageview", "installer",
    "leonmmcoset", "minesweeper", "osver", "paint", "taskmgr", "xiaobai",
    "portablegl",
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
                       "<": "shift-comma", ">": "shift-dot", "$": "shift-4", "?": "shift-slash",
                       "'": "apostrophe", '"': "shift-apostrophe", "!": "shift-1"}
        for character in text:
            self.key(punctuation.get(character, text_keys(character)[0]))
        # The keyboard and USB pointer travel through separate guest queues.
        time.sleep(1)


@contextmanager
def boot(output, disk, iso=None, *, smp=2, firmware="uefi", accel="auto", firmware_image=None):
    output.mkdir(parents=True, exist_ok=True)
    serial = output / "serial.log"
    qmp = ROOT.parent / f".reliefos-inst-{uuid.uuid4().hex[:12]}.sock"
    command = ["qemu-system-x86_64", *qemu_acceleration(accel), "-machine", "q35",
               "-m", "4096", "-smp", str(smp),
               *firmware_arguments(firmware, firmware_image),
               "-display", "none", "-serial", f"file:{serial}",
               "-vga", "std", "-device", "qemu-xhci", "-device", "usb-tablet",
               "-netdev", "user,id=net0", "-device", "e1000,netdev=net0",
               "-drive", f"file={disk},format=raw,if=ide",
               "-qmp", f"unix:{qmp},server=on,wait=off", "-no-reboot", "-no-shutdown"]
    # An installed disk has its own EFI loader. Give the optical device an
    # explicit firmware priority so update tests actually boot the installer.
    command += (["-drive", f"file={iso},format=raw,media=cdrom,if=none,id=installer-cd",
                 "-device", "ide-cd,drive=installer-cd,bootindex=1", "-boot", "d"]
                if iso else ["-boot", "c"])
    (output / "command.json").write_text(json.dumps(command, indent=2) + "\n")
    with (output / "qemu.log").open("w") as errors:
        process = subprocess.Popen(command, stdout=errors, stderr=errors, cwd=ROOT)
        probe = None
        try:
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError("QEMU failed to start; see " + str(output / "qemu.log"))
                try:
                    probe = Probe(qmp, output)
                    break
                except (FileNotFoundError, ConnectionRefusedError):
                    # A Unix socket pathname may precede listen(). Require a
                    # successful handshake, bounded by process state/deadline.
                    time.sleep(.1)
            if probe is None:
                raise TimeoutError("QEMU QMP did not become ready within 15 seconds")
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
    if before == 1 and not getattr(probe, "license_accepted", False):
        probe.click(*probe.license_indicator_point())
        probe.license_accepted = True
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


def select_installation_mode(probe, *, update):
    """Select the actual radio, then decode the sidebar using that route."""
    probe.click(*probe.mode_option_point(update=update))
    probe.installer_update_mode = update


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
    subprocess.run(["debugfs", "-R", f"dump /var/log/installer.log {log_path}",
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
    # The trust package also requires these certificates in console installations.
    xorg_packages.discard("ca-certificates-bundle")
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


def installer_finish_log(probe, serial, process, log_path="/var/log/installer.log"):
    """Read a bounded live log suffix and this request's status via the rescue TTY."""
    start = serial.stat().st_size
    token = "INSTALLER_LOG_READ_DONE_" + uuid.uuid4().hex
    probe.key("ctrl-alt-f2")
    time.sleep(1)
    probe.text(f"tail -c 16384 {log_path} > /dev/ttyS0; "
               f"echo {token}:$? > /dev/ttyS0")
    probe.key("ret")
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        assert process.poll() is None, "QEMU exited before reading installer results"
        with serial.open("r", errors="replace") as stream:
            stream.seek(start)
            text = stream.read()
        result = re.search(re.escape(token) + r":([0-9]+)(?:\r?\n|$)", text)
        if result:
            assert result.group(1) == "0", "Installer log read failed: " + result.group(1)
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
        frame = probe.frame("tty-install-progress" if tty else "progress")
        if tty:
            finished = any(SequenceMatcher(None, "installationresult", text).ratio() >= .78
                           for text in tui_ocr(frame, header="Installation result").splitlines())
            if finished:
                assert "successfully" in tui_ocr(frame), "TUI reported installation failure"
        else:
            finished = probe.visible_page(frame) == 10
        if finished:
            current = installer_finish_log(probe, serial, process)
            assert "[installer.elf] installation completed successfully" in current, current[-10000:]
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


def install_gui(probe, serial, process, *, skip_xorg=False, chinese=False,
                install_bios=True, install_uefi=True, install_timeout=2400,
                swap_mib=None, desktop="icewm", confirm_install=True):
    """Exercise the real wizard, retained choices, and final write boundary."""
    probe.enable_installer_geometry()
    wait_log(serial, "path=/usr/lib/reliefos/apps/installer/installer.elf", process, timeout=300)
    frame = wait_installer_sidebar(probe, process, "language", timeout=300)
    assert probe.visible_page(frame) == 0, "Installer did not start at the language page"
    if chinese:
        probe.click(*probe.language_option_point(chinese=True))
        probe.frame("language-chinese")
    for page in (1, 3, 4):
        next_page(probe, page)
    # Target selection requires an explicit user choice, including on a VM
    # with only one eligible disk.
    probe.click(*probe.disk_selection_point())
    next_page(probe, 5)
    if not install_bios:
        probe.click(*probe.bootloader_indicator_point(bios=True))
    if not install_uefi:
        probe.click(*probe.bootloader_indicator_point(bios=False))
    probe.frame("bootloaders")
    next_page(probe, 6)
    probe.frame("partitions-planned")
    next_page(probe, 11)

    def choose_desktop(index):
        probe.click(*probe.desktop_option_point())
        for _ in range(2):
            probe.key("up")
        for _ in range(index):
            probe.key("down")
        probe.key("ret")
        probe.move(probe.width - 10, 10)

    def desktop_crop(name):
        frame = probe.frame(name)
        x, y = probe.content_left, probe.sidebar_top + 152
        return frame.crop((x + 5, y + 8, x + min(420, probe.content_width - 5), y + 29))

    def verify_desktop(crop, index):
        if chinese and index != 1:
            return
        from test_vt_qemu import ocr
        text = ocr(crop, psm=7).lower()
        expected = "xfce" if chinese else ("default", "experimental", "only")[index]
        assert expected in text, f"Expected {expected} desktop option, saw {text.strip()!r}"

    verify_desktop(desktop_crop("system-desktop-default"), 0)
    choose_desktop(2)
    verify_desktop(desktop_crop("system-desktop-console"), 2)
    previous_page(probe, 6)
    next_page(probe, 11)
    verify_desktop(desktop_crop("system-desktop-retained"), 2)
    selected = 2 if skip_xorg else 1 if desktop == "xfce" else 0
    choose_desktop(selected)
    verify_desktop(desktop_crop("system-desktop-selected"), selected)

    swap_x, swap_y = probe.swap_indicator_point()
    probe.move(probe.width - 10, 10)
    frame = probe.frame("system-swap-default")
    indicator = frame.crop((swap_x - 9, swap_y - 9, swap_x + 10, swap_y + 10))
    assert not xorg_checkbox_checked(indicator), "Swap option is not unchecked by default"
    if swap_mib is not None:
        if not isinstance(swap_mib, int) or not 1 <= swap_mib <= 16777216:
            raise ValueError("swap_mib must be a valid positive MiB count")
        probe.click(swap_x, swap_y)
        probe.click(*probe.swap_size_point())
        probe.key("home")
        probe.key("shift-end")
        probe.text(str(swap_mib))
        probe.move(probe.width - 10, 10)
        frame = probe.frame("system-swap-enabled")
        indicator = frame.crop((swap_x - 9, swap_y - 9, swap_x + 10, swap_y + 10))
        assert xorg_checkbox_checked(indicator), "Swap option did not become checked"

    next_page(probe, 7)
    for index, text in enumerate(("alice", USER_PASSWORD, USER_PASSWORD, ROOT_PASSWORD, ROOT_PASSWORD)):
        probe.click(*probe.account_field_point(index))
        probe.text(text)
    probe.frame("accounts")
    next_page(probe, 8)
    probe.frame("confirmation")
    # Invalid acknowledgement must leave the read-only review in place.
    probe.click(*probe.confirm_field_point())
    probe.text("NO")
    probe.click(*probe.footer_point())
    assert probe.visible_page(probe.frame("confirmation-invalid")) == 8
    if not confirm_install:
        return
    probe.click(*probe.confirm_field_point())
    probe.key("home")
    probe.key("shift-end")
    probe.text("INSTALL")
    probe.frame("confirmation-ready")
    next_page(probe, 9)
    wait_install(probe, serial, process, timeout=install_timeout)


def tui_ocr(frame, *, header=None):
    from PIL import ImageOps
    from test_vt_qemu import ocr
    # The framebuffer console uses 8x16 cells. Match the bounded ncurses
    # viewport while retaining the original, full-resolution evidence file.
    columns, rows = frame.width // 8, frame.height // 16
    width, height = min(columns, 110), min(rows, 36)
    left, top = (columns - width) // 2 * 8, (rows - height) // 2 * 16
    if header:
        image = frame.crop((left + 16, top, left + 16 + len(header) * 8, top + 16))
    else:
        image = frame.crop((left, top, left + width * 8, top + height * 16))
    image = ImageOps.autocontrast(image.convert("L"))
    mode = 7 if header else 6
    text = ocr(image, psm=mode) + ocr(ImageOps.invert(image), psm=mode)
    return "\n".join("".join(line.lower().split()) for line in text.splitlines() if line.strip())


def wait_tui_page(probe, process, title, timeout=90):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        frame = probe.frame("tui-" + title.lower().replace(" ", "-"))
        expected = "".join(title.lower().split())
        candidates = tui_ocr(frame, header=title).splitlines()
        # The fixed console font makes OCR occasionally read W as two l's.
        # Titles remain distinct at this threshold. Completion also requires
        # the live backend success marker and the visible success message.
        if any(SequenceMatcher(None, expected, text).ratio() >= .78 for text in candidates):
            return frame
        if process is not None and process.poll() is not None:
            raise RuntimeError("QEMU exited before TUI page " + title)
        time.sleep(.5)
    raise AssertionError("TUI page did not appear: " + title)


def install_tty(probe, serial, process, components="none", *, skip_xorg=False,
                install_bios=True, install_uefi=True, select_entry=True, install_timeout=2400,
                swap_mib=None, partition_roles=None, confirm_install=True, desktop="icewm"):
    """Operate ncurses controls; partition_roles addresses physical table rows."""
    if swap_mib is not None and (not isinstance(swap_mib, int) or not 1 <= swap_mib <= 16777216):
        raise ValueError("swap_mib must be a valid positive MiB count")
    if partition_roles and any(role not in ("esp", "root", "bios", "data") or
                               not isinstance(row, int) or row < 0 for role, row in partition_roles.items()):
        raise ValueError("invalid partition role or row")
    if select_entry:
        from test_vt_qemu import ocr
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
        probe.text("installer --text")
        probe.key("ret")
    # The first radio always offers English, regardless of the boot locale.
    time.sleep(2)
    probe.key("spc")
    wait_tui_page(probe, process, "Language")
    for title in ("License", "Installation mode", "Target disk"):
        probe.key("f3")
        wait_tui_page(probe, process, title)
    probe.key("spc")  # explicitly select the first target disk
    probe.key("f3")
    wait_tui_page(probe, process, "Bootloaders")
    if not install_bios:
        probe.key("spc")
    probe.key("tab")
    if not install_uefi:
        probe.key("spc")
    probe.key("f3")
    wait_tui_page(probe, process, "Partition layout")
    if partition_roles:
        probe.key("tab")
        probe.key("spc")  # manual scheme
        for role, row in partition_roles.items():
            probe.key("f2")
            probe.key("f3")  # reset control focus, retaining the manual scheme
            for _ in range(2):
                probe.key("tab")
            probe.key("home")
            for _ in range(row):
                probe.key("down")
            for _ in range(9):
                probe.key("tab")  # the final action is Partition role
            probe.key("ret")
            wait_tui_page(probe, process, "Partition role")
            for _ in range({"root": 0, "esp": 1, "bios": 2, "data": 3}[role]):
                probe.key("tab")
            probe.key("spc")
            probe.key("f3")
            wait_tui_page(probe, process, "Partition layout")
    probe.key("f3")
    wait_tui_page(probe, process, "System options")
    desktop_index = 2 if skip_xorg else 1 if desktop == "xfce" else 0
    for _ in range(desktop_index):
        probe.key("tab")
    if desktop_index:
        probe.key("spc")
    for _ in range(3 - desktop_index):
        probe.key("tab")
    if swap_mib is not None:
        probe.key("spc")
        probe.key("tab")
        probe.key("ctrl-u")
        probe.text(str(swap_mib))
    probe.key("f3")
    wait_tui_page(probe, process, "Accounts")
    for value in ("alice", USER_PASSWORD, USER_PASSWORD, ROOT_PASSWORD, ROOT_PASSWORD):
        probe.text(value)
        probe.key("tab")
    probe.key("f3")
    wait_tui_page(probe, process, "Review installation")
    probe.frame("tui-review")
    if not confirm_install:
        probe.text("NO")
        probe.key("f3")
        wait_tui_page(probe, process, "Review installation")
        return
    probe.text("INSTALL")
    probe.key("f3")
    wait_install(probe, serial, process, tty=True, timeout=install_timeout)


def login_tty(probe, serial, process, root, *, skip_xorg=False):
    # Console-profile installs expose tty1 directly. Xorg-profile installs
    # keep a text getty on tty2, even while a graphical greeter owns tty1.
    from test_vt_qemu import wait_text, ocr
    username = "root" if root else "alice"
    if skip_xorg:
        wait_text(probe, process, "console-tty1-login", "login:", timeout=90)
    else:
        # Xorg takes tty1 after init completes. Switch only after that handoff
        # so its startup cannot steal the test's text login back from tty2.
        wait_log(serial, "VT graphics vt=1 enabled=1", process, timeout=120)
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            assert process.poll() is None, "QEMU exited before XDM was ready"
            frame = probe.frame("xdm-ready")
            # The large X logo dominates whole-screen text segmentation.
            greeter = frame.crop((max(0, frame.width // 2 - 320), frame.height // 4,
                                  min(frame.width, frame.width // 2 + 180), frame.height * 3 // 4))
            if "Login into ReliefOS" in ocr(greeter):
                break
            time.sleep(.5)
        else:
            raise AssertionError("XDM login fields did not become ready")
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


def shutdown_after_login(probe, process, *, root):
    """Stop services cleanly so the next boot does not inherit Xorg's live lock."""
    from test_vt_qemu import wait_text
    if not root:
        probe.text("exit")
        probe.key("ret")
        wait_text(probe, process, "shutdown-login", "login:")
        probe.text("root")
        probe.key("ret")
        wait_text(probe, process, "shutdown-password", "Password:")
        probe.text(ROOT_PASSWORD)
        probe.key("ret")
        probe.text("echo SHUTDOWN_AUTHENTICATED")
        probe.key("ret")
        wait_text(probe, process, "shutdown-root", "SHUTDOWN_AUTHENTICATED")
    probe.text("/sbin/poweroff")
    probe.key("ret")
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        status = process.poll()
        if status is not None:
            assert status == 0, f"QEMU exited with status {status} during shutdown"
            return
        if probe.command("query-status")["status"] == "shutdown":
            return
        time.sleep(.2)
    raise AssertionError("The installed system did not finish its normal shutdown")


def login_desktop(probe, serial, process):
    from test_xorg_vm_qemu import wait_greeter_stage
    wait_greeter_stage(probe, process, "login", "login-ready", timeout=90)
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
    time.sleep(3)
    probe.text("echo INSTALLER_ST_READY >/dev/ttyS0")
    probe.key("ret")
    wait_log(serial, "INSTALLER_ST_READY", process)
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
    assert "INSTALLER_ST_READY" in log, "X11 st shell did not become interactive"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", type=Path, default=ROOT / "out/x86_64/release/images/reliefos-installer.iso")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--installer", choices=("gui", "tty"), default="gui")
    parser.add_argument("--accel", choices=("auto", "kvm", "tcg"), default="auto")
    parser.add_argument("--firmware", choices=("bios", "uefi"), default="uefi")
    parser.add_argument("--firmware-image", type=Path, help="Read-only OVMF firmware override")
    parser.add_argument("--without-xorg", action="store_true",
                        help="Skip the optional Xorg desktop during a fresh install")
    parser.add_argument("--swap-mib", type=int, help="Enable a swapfile with this explicit MiB size")
    parser.add_argument("--login-only", action="store_true", help="Boot this test's existing scratch disk without reinstalling")
    parser.add_argument("--desktop", action="store_true", help="Also log into the installed desktop and capture Terminal identity")
    parser.add_argument("--desktop-only", action="store_true", help="Only boot the existing test disk into the desktop")
    args = parser.parse_args()
    if args.swap_mib is not None and not 1 <= args.swap_mib <= 16777216:
        parser.error("--swap-mib must be between 1 and 16777216")
    if args.without_xorg and (args.desktop or args.desktop_only):
        parser.error("--desktop and --desktop-only require Xorg")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    disk = output / "scratch.raw"
    guest_options = {"accel": args.accel, "firmware": args.firmware,
                     "firmware_image": args.firmware_image}
    if args.desktop_only:
        if not disk.is_file(): parser.error("--desktop-only requires an existing scratch disk")
        with boot(output / "desktop", disk, **guest_options) as session:
            login_desktop(*session)
        return
    if args.login_only:
        if not disk.is_file(): parser.error("--login-only requires an existing scratch disk")
    else:
        with disk.open("xb") as file: file.truncate(8 * 1024**3)
        with boot(output / "install", disk, args.iso.resolve(), **guest_options) as session:
            if args.installer == "gui":
                install_gui(*session, skip_xorg=args.without_xorg, swap_mib=args.swap_mib)
            else:
                install_tty(*session, skip_xorg=args.without_xorg, swap_mib=args.swap_mib)
    with boot(output / "root", disk, **guest_options) as session:
        login_tty(*session, True, skip_xorg=args.without_xorg)
        shutdown_after_login(session[0], session[2], root=True)
    with boot(output / "user", disk, **guest_options) as session:
        login_tty(*session, False, skip_xorg=args.without_xorg)
        shutdown_after_login(session[0], session[2], root=False)
    if args.desktop:
        with boot(output / "desktop", disk, **guest_options) as session:
            login_desktop(*session)
    print(f"PASS: installer, ordinary/root credentials: {output}")


if __name__ == "__main__":
    main()
