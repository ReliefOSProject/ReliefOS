#!/usr/bin/env python3
"""Verify global lock state through login, desktop routing and Terminal in QEMU."""
import argparse
from pathlib import Path
import subprocess
import time

from test_pam_desktop_qemu import root_partition
from test_installer_accounts_qemu import boot, wait_log
from test_sudo_e2e_qemu import Probe
from reliefos_layout import app_exec_path_abs

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disk", type=Path, required=True, help="raw disk with test/test login")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists() or not output.is_relative_to(ROOT / "build"):
        parser.error("output must be a new directory below build")
    output.mkdir(parents=True)
    disk = output / "scratch.raw"
    subprocess.run(["cp", "--reflink=auto", str(args.disk.resolve()), str(disk)], check=True)
    for source, target in (("build/system/kernel.sys", "::/leonos/kernel.sys"),
                           ("build/system/kernel.sys", "::/reliefos/kernel.sys"),
                           ("boot/grub/grub.cfg", "::/grub/grub.cfg")):
        subprocess.run(["mcopy", "-o", "-i", f"{disk}@@1048576", str(ROOT / source), target], check=True)
    filesystem = output / "root.ext2"
    start = root_partition(disk, filesystem)
    replacements = [(ROOT / "build/system/lib/libreliefos.so.2", "/usr/lib/reliefos/libreliefos.so.2"),
                    (ROOT / "build/system/lib/libleonos.so.2", "/usr/lib/leonos/libleonos.so.2")]
    replacements += [(ROOT / f"build/userland/{name}.elf", app_exec_path_abs(name))
                     for name in ("desktop", "windowd", "terminal", "calc", "login")]
    for source, target in replacements:
        for command in (f"rm {target}", f'write "{source}" {target}', f"set_inode_field {target} mode 0100755"):
            subprocess.run(["debugfs", "-w", "-R", command, str(filesystem)], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["dd", f"if={filesystem}", f"of={disk}", "bs=4M", f"seek={start}B",
                    "conv=notrunc", "status=none"], check=True)
    with boot(output, disk) as (probe, serial, process):
        probe.__class__ = Probe
        wait_log(serial, "[login.elf] starting login UI", process, timeout=60)
        time.sleep(3)
        probe.key("caps_lock")
        probe.key("down")
        # Caps Lock + Shift gives the lowercase password, even in a new editor.
        for letter in "test":
            probe.key("shift-" + letter)
        probe.key("ret")
        wait_log(serial, "[pam-login] session ready", process, timeout=30)
        time.sleep(3)
        probe.key("meta_l")
        for letter in "terminal":
            probe.key("shift-" + letter)
        probe.key("ret")
        wait_log(serial, "terminal: PTY ready", process, timeout=30)
        time.sleep(3)
        # This Terminal was created with Caps Lock already active.
        for letter in "echo":
            probe.key("shift-" + letter)
        probe.key("spc")
        probe.text("abc")
        probe.key("caps_lock")
        probe.text("abc > /tmp/capslock-first; sync")
        probe.key("ret")
        wait_log(serial, "name=sync code=0", process, timeout=30)
        probe.frame("first-terminal")
        # Change the global lock while another window owns keyboard focus.
        probe.key("meta_l")
        probe.key("caps_lock")
        probe.click(600, 400)
        time.sleep(1)
        for letter in "echo":
            probe.key("shift-" + letter)
        probe.key("spc")
        probe.text("xyz")
        probe.key("caps_lock")
        probe.text("xyz > /tmp/capslock-second; sync")
        probe.key("ret")
        time.sleep(4)
        probe.text("cat /tmp/capslock-first /tmp/capslock-second; sync")
        probe.key("ret")
        time.sleep(4)
        probe.frame("capslock-results")
    root_partition(disk, filesystem)
    for name, expected in (("first", "ABCabc\n"), ("second", "XYZxyz\n")):
        result = subprocess.check_output(["debugfs", "-R", f"cat /tmp/capslock-{name}", str(filesystem)],
                                         stderr=subprocess.DEVNULL).decode()
        (output / f"capslock-{name}.txt").write_text(result)
        assert result == expected, (name, repr(result), repr(expected))
    print("PASS Caps Lock: login Shift XOR, new Terminal and focus changes")


if __name__ == "__main__":
    main()
