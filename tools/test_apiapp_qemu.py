#!/usr/bin/env python3
"""Exercise the API wizard and real sudo/PAM on a disposable Chinese live root."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time

from PIL import ImageChops
from build_api import build_api_file
import test_linux_ioctl_cloexec as iso_tools
from test_network_qemu import inject
from test_sudo_e2e_qemu import Probe
from test_installer_window_qemu import title_point, wait_log

ROOT = Path(__file__).resolve().parents[1]
APP = "/usr/lib/reliefos/apps/apiapp/apiapp.elf"


def wait_since(serial, offset, needle, process, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = serial.read_text(errors="replace")[offset:]
        if re.search(needle, text):
            return text
        if process.poll() is not None:
            raise AssertionError("QEMU exited unexpectedly")
        time.sleep(.2)
    raise AssertionError(f"Missing serial output: {needle}")


def app_execs(text):
    return len(set(re.findall(r"exec pid=(\d+) path=" + re.escape(APP), text)))


def check_wizard(probe, serial, process, terminal_title, package, outcome):
    offset = len(serial.read_text(errors="replace"))
    probe.click(*terminal_title)
    probe.text(f"sudo -k; {APP} /api/{package}.api")
    probe.key("ret")
    wait_since(serial, offset, re.escape(f"path={APP}"), process)
    time.sleep(2)
    frame = probe.frame(f"{outcome}-welcome")
    x, y = title_point(frame)
    authorization_offset = len(serial.read_text(errors="replace"))
    # The title bar is directly above the 480x360 client, with a 24px offset.
    button = (x - 120 + 400, y - 8 + 344)
    probe.click(*button)
    time.sleep(.5)
    probe.click(*button)
    wait_since(serial, authorization_offset, re.escape("path=/usr/lib/reliefos/apps/sudod/sudod.elf"), process)
    time.sleep(1)
    probe.frame(f"{outcome}-authorization-zh")
    if outcome == "cancel":
        probe.key("esc")
    else:
        probe.text("test")
        probe.key("ret")
    wait_since(serial, authorization_offset, rf"name=sudo code={0 if outcome == 'success' else 1}\b", process)
    time.sleep(2)
    finished = probe.frame(f"{outcome}-finished")
    text = serial.read_text(errors="replace")[offset:]
    expected = 1 if outcome == "cancel" else 2
    assert app_execs(text) == expected, f"unexpected API process count: {app_execs(text)}"
    if outcome == "cancel":
        assert "path=/sbin/unix_chkpwd" not in text, "cancel submitted a password"
    else:
        assert re.search(r"name=unix_chkpwd code=0\b", text), "PAM did not authenticate"
        assert re.search(rf"name=apiapp\.elf code={0 if outcome == 'success' else 1}\b", text), "worker did not finish"
    time.sleep(6)
    settled = probe.frame(f"{outcome}-after-wait")
    assert app_execs(serial.read_text(errors="replace")[offset:]) == expected, "wizard relaunched itself"
    area = (x - 120, y - 8, x - 120 + 480, y - 8 + 380)
    assert ImageChops.difference(finished.crop(area), settled.crop(area)).getbbox() is None, "result window changed without input"
    close_offset = len(serial.read_text(errors="replace"))
    probe.click(*button)
    wait_since(serial, close_offset, rf"name=apiapp\.elf code={0 if outcome == 'success' else 1}\b", process)
    time.sleep(1)
    print(f"[apiapp-gui] PASS {outcome}: one wizard, child reaped, stable result, Close exits", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reuse", action="store_true", help="reuse this test's staged ISO")
    args = parser.parse_args()
    work = ROOT / "build/apiapp-gui-test"
    work.mkdir(parents=True, exist_ok=True)
    iso = work / "apiapp-test.iso"
    if not args.reuse:
        image = work / "root.ext2"
        shutil.copy2(ROOT / "build/live/root.ext2", image)
        compiler = ROOT / "build/musl-gcc/root/opt/dyne/gcc-musl/bin/x86_64-linux-musl-gcc"
        pipe_probe = work / "pipe-stat-test"
        subprocess.run([str(compiler), "-static", "-O2", "-Wall", "-Wextra",
                        str(ROOT / "kernel/reliefnt/tools/tests/pipe_stat_type_test.c"), "-o", str(pipe_probe)], check=True)
        subprocess.run([str(pipe_probe)], check=True, timeout=10)
        inject(image, pipe_probe, "/bin/pipe-stat-test", "0100755")
        locale = work / "locale.conf"
        locale.write_text("lang=zh\n", encoding="ascii")
        inject(image, locale, "/etc/reliefos/locale.conf")
        # A real file/directory collision forces extraction failure. The
        # package and installer are otherwise the production implementations.
        collision = work / "collision"
        collision.write_text("installation target is a regular file\n", encoding="ascii")
        inject(image, collision, "/usr/lib/reliefos/apps/api-failure")
        for name in ("failure", "success"):
            package = work / f"{name}.api"
            build_api_file(f"API {name} fixture", "1.0", "fixture.elf",
                           f"/usr/lib/reliefos/apps/api-{name}", True, False, "",
                           [(str(ROOT / "build/userland/helloworld.elf"), "fixture.elf")],
                           str(package), app_id=f"api-{name}")
            inject(image, package, f"/api/{name}.api")
        iso_tools.GRUB_TEMPLATE = iso_tools.GRUB_TEMPLATE.replace("set timeout=5", "set timeout=0").replace(
            "autospawn=ioctlcloexec autospawn=python315", "").replace("syscall-trace=/opt/python/", "")
        iso_tools.build_iso(image, iso, work / "grub.cfg", work)
    serial = work / "serial.log"
    serial.unlink(missing_ok=True)
    with tempfile.TemporaryDirectory(prefix="api-qmp-") as temporary, (work / "qemu.log").open("w") as errors:
        qmp = Path(temporary) / "qmp"
        command = ["qemu-system-x86_64", "-enable-kvm", "-cpu", "host", "-machine", "q35", "-m", "4096",
                   "-smp", "2", "-bios", "/usr/share/edk2/x64/OVMF.4m.fd", "-display", "none",
                   "-serial", f"file:{serial}", "-device", "VGA,xres=1280,yres=720", "-cdrom", str(iso),
                   "-boot", "d", "-nic", "none", "-qmp", f"unix:{qmp},server=on,wait=off", "-no-reboot", "-no-shutdown"]
        process = subprocess.Popen(command, stdout=errors, stderr=errors, cwd=ROOT)
        probe = None
        try:
            wait_log(serial, "[login.elf] starting login UI", process, timeout=60)
            probe = Probe(qmp, work)
            time.sleep(2)
            probe.key("down")
            probe.text("test")
            probe.key("ret")
            wait_log(serial, "[pam-login] session ready", process, timeout=30)
            time.sleep(2)
            probe.key("meta_l")
            time.sleep(1)
            probe.text("terminal")
            probe.key("ret")
            wait_log(serial, "terminal: PTY ready", process, timeout=30)
            time.sleep(2)
            terminal_title = title_point(probe.frame("terminal-ready"))
            pipe_offset = len(serial.read_text(errors="replace"))
            probe.text("/bin/pipe-stat-test")
            probe.key("ret")
            wait_since(serial, pipe_offset, r"name=pipe-stat-test code=0\b", process)
            probe.frame("pipe-stat-passed")
            print("[apiapp-gui] PASS guest raw pipe stat probe", flush=True)
            for package, outcome in (("failure", "cancel"), ("failure", "failure"), ("success", "success")):
                check_wizard(probe, serial, process, terminal_title, package, outcome)
            assert "KERNEL PANIC" not in serial.read_text(errors="replace")
        finally:
            if probe is not None:
                try:
                    probe.frame("last-frame")
                finally:
                    probe.close()
            if process.poll() is None:
                iso_tools.qmp_quit(qmp, process)


if __name__ == "__main__":
    main()
