#!/usr/bin/env python3
"""One-off interactive probe of the XDM greeter and the VT hand-off.

Boots the Xorg image, drives the xlogin greeter one keystroke group at a time
with an OCR snapshot after each, then switches to tty2 and collects the XDM and
Xorg logs. Not part of the test suite.
"""
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_xorg_vm_qemu import boot, login, collect, IMAGE, OUTPUT  # noqa: E402
from test_installer_accounts_qemu import Probe  # noqa: E402
from test_installer_window_qemu import wait_log  # noqa: E402
from test_vt_qemu import wait_text, ocr  # noqa: E402
from run_gcc_probe_qemu import qmp_quit  # noqa: E402


def snap(probe, name):
    visible = ocr(probe.frame(name))
    print(f"--- {name}: {visible!r}", flush=True)
    return visible.lower()


def main() -> int:
    process, serial, qmp = boot([], IMAGE, OUTPUT)
    probe = None
    try:
        deadline = time.monotonic() + 15
        while not qmp.exists() and time.monotonic() < deadline:
            time.sleep(.1)
        probe = Probe(qmp, OUTPUT)
        wait_log(serial, "boot complete:", process, timeout=120)
        print("probe: boot complete", flush=True)
        wait_text(probe, process, "greeter", "login:", timeout=90)
        snap(probe, "g0")

        probe.text("test")
        probe.key("ret")
        time.sleep(1)
        state = snap(probe, "g1-after-name")
        if "password:" not in state:
            print("probe: no password prompt after name+ret; trying tab then ret", flush=True)
            probe.key("tab")
            probe.key("ret")
            time.sleep(1)
            state = snap(probe, "g1b-after-tab")
        if "password:" in state:
            probe.text("test")
            probe.key("ret")
            time.sleep(2)
            snap(probe, "g2-after-password")
            time.sleep(4)
            snap(probe, "g3-later")

        probe.key("ctrl-alt-f2")
        time.sleep(2)
        state = snap(probe, "tty2-after-switch")
        if "login:" in state:
            login(probe, process, "probe-tty2", "root", "root", "login:")
            for tag, path in (("XDM-LOG", "/var/log/xdm.log"),
                              ("XORG-LOG", "/var/log/Xorg.0.log")):
                print(f"===== {tag} =====", flush=True)
                print(collect(probe, serial, process, tag, path), flush=True)
        else:
            print("probe: tty2 has no login prompt; dumping serial tail", flush=True)
            print(serial.read_text(errors="replace")[-3000:], flush=True)
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
