#!/usr/bin/env python3
"""Run GTK programs in the XDM guest session and require clean startup.

Boots the Xorg image (XORG_IMAGE, default out/x86_64/release/images/reliefos.raw),
signs in at the xlogin greeter, and starts the shipped GTK executables from the
session xterm.  Each program must still be alive once its startup window had
time to map, and its stderr must not carry GLib/GTK warnings or errors such as
the missing-pixbuf-loader message.  glycin's plain "Glycin running without
sandbox" notice is evidence, not a failure: the guest kernel has no
unprivileged user namespaces, and upstream glycin degrades to unsandboxed
loader processes for that case.
"""
from pathlib import Path
import os
import sys
import time

from test_xorg_vm_qemu import boot, collect, greeter_login, wait_session, xterm_marker
from test_installer_window_qemu import wait_log
from test_installer_accounts_qemu import Probe
from run_gcc_probe_qemu import qmp_quit

ROOT = Path(__file__).resolve().parents[1]
IMAGE = Path(os.environ.get("XORG_IMAGE",
                            ROOT / "out/x86_64/release/images/reliefos.raw"))
OUTPUT = Path(os.environ.get("GTK_OUTPUT", ROOT / "build/gtk-qemu"))

# GTK programs shipped in the image that link against the GTK stack, mapped to
# the command line that opens a real window.  lxshortcut is a desktop-entry
# editor: without --input it exits with "You need at least one option".
APPS = {
    "libfm-pref-apps": "libfm-pref-apps",
    "lxshortcut": "lxshortcut --input /usr/share/applications/lxshortcut.desktop",
}

# Click point that hands IceWM's pointer-bound keyboard focus back to the
# session xterm: well inside the +0+0 terminal, clear of the centered GTK
# dialogs that map around the middle of the screen.
FOCUS_X, FOCUS_Y = 120, 120

# Glib/GTK diagnostics that must never appear on a healthy run.  The glycin
# sandbox notice ("WARNING: Glycin running without sandbox.") is deliberately
# not listed; it is recorded in the evidence output instead.
FORBIDDEN_LOG_STRINGS = ("Gtk-WARNING", "WARNING **", "ERROR **",
                         "Could not load a pixbuf", "doesn't exist, ignoring it")


def main() -> int:
    if not IMAGE.is_file():
        print(f"gtk-qemu: image not built: {IMAGE}")
        return 1
    if OUTPUT.exists():
        print(f"gtk-qemu: output directory already exists: {OUTPUT}")
        return 1
    OUTPUT.mkdir(parents=True)
    failures = []

    def fail(message):
        print(f"gtk-qemu: FAIL {message}", flush=True)
        failures.append(message)

    process, serial, qmp = boot([], IMAGE, OUTPUT)
    probe = None
    evidence = []
    try:
        deadline = time.monotonic() + 15
        while not qmp.exists() and time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError("QEMU exited before QMP became ready")
            time.sleep(.1)
        probe = Probe(qmp, OUTPUT)
        wait_log(serial, "boot complete:", process, timeout=120)
        print("gtk-qemu: boot complete", flush=True)

        greeter_login(probe, process)
        wait_session(probe, process, "tty1-session")
        xterm_marker(probe, serial, process, "GTK-XTERM-READY")
        print("gtk-qemu: session ready", flush=True)

        evidence = []
        for app, launch in APPS.items():
            tag = app.upper()
            log_path = f"/tmp/{tag}.log"
            # IceWM hands the keyboard to a window when it maps and keeps it
            # until the pointer moves, so keystrokes typed while the startup
            # window appears are lost mid-command.  Type only where no window
            # can map in the middle: the launch keystrokes end with the ret
            # that starts the program, its window maps while the guest sleeps,
            # and the program is dead again before the next command is typed.
            probe.click(FOCUS_X, FOCUS_Y)
            table = collect(probe, serial, process, f"{tag}-PS",
                            command=f"{launch} >{log_path} 2>&1 & sleep 25; ps",
                            timeout=45)
            alive = any(app in line for line in table.splitlines())
            if alive:
                print(f"gtk-qemu: {app} is running", flush=True)
            else:
                fail(f"{app} exited before its startup window could map")
            probe.frame(f"{tag.lower()}-window")
            probe.click(FOCUS_X, FOCUS_Y)
            collect(probe, serial, process, f"{tag}-STOP",
                    command=f"killall -9 {app}; sleep 2")
            probe.click(FOCUS_X, FOCUS_Y)
            log = collect(probe, serial, process, f"{tag}-LOG",
                          command=f"cat {log_path} 2>/dev/ttyS0")
            if "can't open" in log or "No such file" in log:
                fail(f"{app} stderr log {log_path} was not captured")
            (OUTPUT / f"{tag.lower()}.log").write_text(log + "\n")
            evidence.append(f"===== {app} stderr =====\n{log}\n")
            for needle in FORBIDDEN_LOG_STRINGS:
                if needle in log:
                    fail(f"{app} stderr contains {needle!r}")
    finally:
        if probe is not None:
            try:
                probe.frame("last-frame")
            finally:
                probe.close()
        if process.poll() is None:
            qmp_quit(qmp, process)
        qmp.unlink(missing_ok=True)

    (OUTPUT / "gtk-evidence.log").write_text("".join(evidence))
    if failures:
        print(f"gtk-qemu: FAIL ({len(failures)} problems)")
        return 1
    print("gtk-qemu: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
