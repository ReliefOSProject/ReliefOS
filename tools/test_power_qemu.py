#!/usr/bin/env python3
"""Click real power controls and require guest-originated QEMU power events.

The installer case erases only a newly created disposable disk in --output,
performs a full GUI installation, ejects the ISO, and boots the installed disk.
"""
import argparse
import json
from pathlib import Path
import select
import subprocess
import time

from run_gcc_probe_qemu import qmp_quit
from test_installer_window_qemu import Probe, ROOT, wait_log


class PowerProbe(Probe):
    def __init__(self, path, output):
        self.events = []
        super().__init__(path, output)

    def click(self, x, y):
        self.move(x, y)
        time.sleep(0.2)
        self.button(True)
        time.sleep(0.15)
        self.button(False)
        time.sleep(0.2)

    def read_reply(self):
        line = self.stream.readline()
        if not line:
            raise EOFError("QMP closed")
        reply = json.loads(line)
        if "event" in reply:
            self.events.append(reply)
        return reply

    def command(self, name, arguments=None):
        request = {"execute": name}
        if arguments is not None:
            request["arguments"] = arguments
        self.stream.write(json.dumps(request).encode() + b"\n")
        while True:
            reply = self.read_reply()
            if "error" in reply:
                raise RuntimeError(reply)
            if "return" in reply:
                return reply["return"]

    def expect_transition(self, name):
        deadline = time.monotonic() + 20
        while True:
            for event in self.events:
                if event["event"] == name and event.get("data", {}).get("guest"):
                    return event
            if time.monotonic() >= deadline:
                raise AssertionError(f"No guest {name} event: {self.events}")
            if not select.select([self.socket], [], [], max(0, deadline - time.monotonic()))[0]:
                continue
            self.read_reply()


def menu_power(probe, action):
    probe.key("meta_l")
    time.sleep(1)
    probe.frame("start-menu")
    probe.click(410, probe.height - 54)
    time.sleep(1)
    probe.frame("power-menu")
    # Default menu geometry: taskbar 34, panel 448, header/tabs/search 111.
    row_y = probe.height - 34 - 448 + 111 + 18 + 13
    probe.click(180, row_y + (31 if action == "shutdown" else 0))
    time.sleep(1)
    frame = probe.frame("power-confirm")
    assert frame.getpixel((probe.width // 2, probe.height // 2 - 60)) == (0, 120, 212), "Power confirmation not visible"


def desktop_power(probe, serial, process, action):
    wait_log(serial, "[oobe.elf] starting first-run", process)
    time.sleep(8)
    probe.text("nanotest")
    probe.key("ret")
    time.sleep(8)
    probe.text("nanotest")
    probe.key("ret")
    time.sleep(5)
    menu_power(probe, action)
    # Cancel must leave the machine running and permit opening the dialog again.
    probe.click(probe.width // 2 + 128, probe.height // 2 + 48)
    time.sleep(1)
    frame = probe.frame("power-cancelled")
    assert frame.getpixel((probe.width // 2, probe.height // 2 - 60)) != (0, 120, 212), "Cancel did not dismiss power confirmation"
    assert "requested from Start menu" not in serial.read_text(errors="replace")
    menu_power(probe, action)
    probe.events.clear()
    try:
        probe.click(probe.width // 2 + 48, probe.height // 2 + 48)
        if action != "shutdown":
            probe.frame("power-clicked")
    except (EOFError, BrokenPipeError):
        if action != "shutdown":
            raise
    event = probe.expect_transition("SHUTDOWN" if action == "shutdown" else "RESET")
    text = serial.read_text(errors="replace")
    assert f"{'shutdown' if action == 'shutdown' else 'restart'} requested from Start menu" in text
    assert f"command=0x{'4321fedc' if action == 'shutdown' else '1234567'}" in text
    if action == "shutdown":
        assert event["data"]["reason"] == "guest-shutdown"
        assert process.wait(timeout=15) == 0
    else:
        assert event["data"]["reason"] == "guest-reset"
        deadline = time.monotonic() + 90
        while sum(serial.read_text(errors="replace").count(marker)
                  for marker in ("[reliefnt] boot complete:", "[ntclks] boot complete:")) < 2:
            assert time.monotonic() < deadline, "Reset did not reach a second kernel boot"
            assert process.poll() is None
            time.sleep(0.2)
        time.sleep(5)
        probe.frame("rebooted")
    return event


def installer_power(probe, serial, process):
    wait_log(serial, "name=installer.elf", process)
    time.sleep(8)
    assert probe.visible_page(probe.frame("installer-language")) == 0
    for expected in (1, 2, 3, 4, 5):
        probe.click(*probe.footer_point())
        time.sleep(0.8)
        assert probe.visible_page(probe.frame(f"installer-page-{expected}")) == expected
    probe.click(*probe.partition_selection_point())
    probe.click(*probe.footer_point())
    time.sleep(0.8)
    assert probe.visible_page(probe.frame("installer-accounts")) == 6
    for index, value in enumerate(("alice", "password", "password", "rootpass", "rootpass")):
        probe.click(*probe.account_field_point(index))
        probe.text(value)
    probe.click(*probe.footer_point())
    time.sleep(0.8)
    assert probe.visible_page(probe.frame("installer-confirm")) == 7
    probe.click(*probe.confirm_field_point())
    probe.text("INSTALL")
    time.sleep(1)
    probe.frame("installer-confirm-typed")
    probe.click(*probe.footer_point())
    time.sleep(2)
    assert probe.visible_page(probe.frame("installer-copying")) == 8, "Install did not start"
    print("GUI installation started on disposable disk", flush=True)
    return complete_installer_power(probe, serial, process)


def complete_installer_power(probe, serial, process):
    wait_log(serial, "[installer.elf] installation completed successfully", process, 1800)
    time.sleep(2)
    assert probe.visible_page(probe.frame("installer-complete")) == 9
    probe.command("eject", {"device": "installcd", "force": True})
    probe.events.clear()
    probe.click(*probe.footer_point())
    event = probe.expect_transition("RESET")
    assert event["data"]["reason"] == "guest-reset"
    assert "restart requested from completion page" in serial.read_text(errors="replace")
    wait_log(serial, "[oobe.elf] starting first-run", process, 90)
    text = serial.read_text(errors="replace")
    assert sum(text.count(marker)
               for marker in ("[reliefnt] boot complete:", "[ntclks] boot complete:")) >= 2
    assert "fs=ext2 desktop=desktop.elf" in text
    time.sleep(8)
    probe.frame("installed-disk-rebooted")
    return event


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("case", choices=("desktop-reboot", "desktop-shutdown", "installer-reboot"))
    parser.add_argument("--firmware", choices=("uefi", "bios"), default="uefi")
    parser.add_argument("--output", type=Path, default=ROOT / "build/power-regressions")
    parser.add_argument("--iso", type=Path)
    args = parser.parse_args()
    out = (args.output / f"{args.case}-{args.firmware}").resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / "result.json").unlink(missing_ok=True)
    serial, qmp = out / "serial.log", out / "qmp.sock"
    qmp.unlink(missing_ok=True)
    is_installer = args.case == "installer-reboot"
    iso = args.iso or ROOT / "build/images" / ("reliefos-installer.iso" if is_installer else "reliefos-live.iso")
    command = ["qemu-system-x86_64", "-enable-kvm", "-cpu", "host", "-machine", "q35",
               "-m", "4096", "-smp", "2", "-display", "none", "-serial", f"file:{serial}",
               "-device", "VGA,xres=1280,yres=720", "-device", "qemu-xhci", "-device", "usb-tablet",
               "-netdev", "user,id=net0", "-device", "e1000,netdev=net0",
               "-qmp", f"unix:{qmp},server=on,wait=off",
               "-drive", f"file={iso.resolve()},format=raw,media=cdrom,if=ide,id=installcd", "-boot", "d"]
    if args.firmware == "uefi":
        command += ["-bios", "/usr/share/edk2/x64/OVMF.4m.fd"]
    if is_installer:
        disk = out / "installed.raw"
        # Refuse to erase any disk retained by a previous run.
        with disk.open("xb") as target:
            target.truncate(2 * 1024**3)
        command += ["-drive", f"file={disk},format=raw,if=ide"]
    with (out / "qemu.log").open("w") as errors:
        process = subprocess.Popen(command, stdout=errors, stderr=errors, cwd=ROOT)
        probe = None
        try:
            deadline = time.monotonic() + 15
            while not qmp.exists() and time.monotonic() < deadline:
                assert process.poll() is None, "QEMU failed to start"
                time.sleep(0.1)
            probe = PowerProbe(qmp, out)
            if is_installer:
                event = installer_power(probe, serial, process)
            else:
                event = desktop_power(probe, serial, process, args.case.split("-")[1])
            assert "KERNEL PANIC" not in serial.read_text(errors="replace")
            (out / "result.json").write_text(json.dumps({
                "case": args.case, "firmware": args.firmware, "power_event": event,
                "iso": str(iso.resolve()), "result": "PASS",
            }, indent=2) + "\n")
            print(f"PASS {args.case} {args.firmware}: {out}", flush=True)
        finally:
            if probe is not None:
                if process.poll() is None:
                    try:
                        probe.frame("final-state")
                        registers = probe.command("human-monitor-command", {"command-line": "info registers -a"})
                        (out / "registers.txt").write_text(registers)
                    except (EOFError, OSError):
                        pass
                (out / "events.json").write_text(json.dumps(probe.events, indent=2) + "\n")
                probe.close()
            if process.poll() is None:
                qmp_quit(qmp, process)
            qmp.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
