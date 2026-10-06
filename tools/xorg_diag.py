#!/usr/bin/env python3
"""One-off diagnostic boot: collect XDM/Xorg failure logs over serial.

Boots the same image test_xorg_vm_qemu.py uses; because Xorg currently dies
during init, tty1 falls back to the text login, which is where the logs are
read from. Not part of the test suite.
"""
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_xorg_vm_qemu import boot, login, IMAGE, OUTPUT  # noqa: E402
from test_installer_accounts_qemu import Probe  # noqa: E402
from test_installer_window_qemu import wait_log  # noqa: E402
from test_vt_qemu import wait_text, ocr  # noqa: E402
from run_gcc_probe_qemu import qmp_quit  # noqa: E402


def run(probe, serial, process, tag, command):
    """Run a guest shell command and return its serial-marked output."""
    begin, end = f"=== {tag} BEGIN ===", f"=== {tag} END ==="
    probe.text(f"echo '{begin}' >/dev/ttyS0")
    probe.key("ret")
    wait_log(serial, begin, process, timeout=15)
    probe.text(f"{command} >/dev/ttyS0")
    probe.key("ret")
    probe.text(f"echo '{end}' >/dev/ttyS0")
    probe.key("ret")
    wait_log(serial, end, process, timeout=15)
    text = serial.read_text(errors="replace")
    return text.split(begin, 1)[1].split(end, 1)[0].strip("\n")


def main() -> int:
    process, serial, qmp = boot([], IMAGE, OUTPUT)
    probe = None
    try:
        deadline = time.monotonic() + 15
        while not qmp.exists() and time.monotonic() < deadline:
            time.sleep(.1)
        probe = Probe(qmp, OUTPUT)
        wait_log(serial, "boot complete:", process, timeout=120)
        print("diag: boot complete", flush=True)
        wait_text(probe, process, "tty1-text", "login:", timeout=60)
        print("diag: tty1 text login:", repr(ocr(probe.frame("tty1-text"))[:200]), flush=True)
        login(probe, process, "tty1-root", "root", "root", "login:")
        for tag, command in (("VARLOG-LIST", "ls -la /var/log"),
                             ("XDM-LOG", "cat /var/log/xdm.log"),
                             ("XORG-LOG", "cat /var/log/Xorg.0.log"),
                             ("XORG-FIRST", "cat /var/log/Xorg.0.log.first"),
                             ("XSESSION-LOG", "cat /var/log/xorg-session.log"),
                             ("AUTHDIR", "ls -la /var/lib/xdm/authdir/authfiles"),
                             ("MKTEMP-TEST",
                              "busybox mktemp /var/lib/xdm/authdir/authfiles/A:0-XXXXXX; "
                              "echo mktemp-exit=$?; "
                              "touch /var/lib/xdm/authdir/authfiles/A:0-TEST; echo touch-exit=$?")):
            print(f"===== {tag} =====", flush=True)
            print(run(probe, serial, process, tag, command), flush=True)
        return 0
    finally:
        if probe is not None:
            try:
                probe.frame("last-frame")
            finally:
                probe.close()
        if process.poll() is None:
            qmp_quit(qmp, process)
        qmp.unlink(missing_ok=True)


if __name__ == "__main__":
    raise SystemExit(main())
