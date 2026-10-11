#!/usr/bin/env python3
"""Profile real desktop composition while an unmodified glxgears window animates."""
import argparse
import re
import subprocess
import time
from pathlib import Path

from test_pam_desktop_qemu import root_partition
from test_installer_accounts_qemu import boot, wait_log
from test_sudo_e2e_qemu import Probe
from reliefos_layout import app_exec_path_abs

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disk", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
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
                    (ROOT / "build/system/lib/libleonos.so.2", "/usr/lib/leonos/libleonos.so.2"),
                    (Path(__file__).resolve(), "/etc/reliefos/desktop-profile")]
    replacements += [(ROOT / f"build/userland/{name}.elf", app_exec_path_abs(name))
                     for name in ("desktop", "windowd", "terminal", "calc", "login", "glxgears")]
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
        probe.key("down")
        probe.text("test")
        probe.key("ret")
        wait_log(serial, "[pam-login] session ready", process, timeout=30)
        time.sleep(3)
        probe.key("meta_l")
        probe.text("glxgears")
        probe.key("ret")
        time.sleep(10)
        probe.frame("gears-start")
        begin = len(serial.read_text(errors="replace"))
        time.sleep(30)
        probe.frame("gears-end")
        sample = serial.read_text(errors="replace")[begin:]
    results = re.findall(r"\[desktop-perf\] frames=(\d+) elapsed_ms=(\d+) paint_ms=(\d+) inputm_ms=(\d+)", sample)
    assert len(results) >= 3, "insufficient compositor samples"
    for frames, elapsed, paint, inputm in results:
        print(f"compositor={int(frames)*1000/int(elapsed):.2f} fps; paint={paint} ms inputm={inputm} ms / {elapsed} ms")


if __name__ == "__main__":
    main()
