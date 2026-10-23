#!/usr/bin/env python3
"""Exercise Alpine HyFetch's wizard and ReliefOS defaults with the legacy logo alias."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

from apk_distribution import ROOT, bootstrap, run
from make_live_root import make_live_tree
from make_ext2_root import write_ext2_root
import test_linux_ioctl_cloexec as iso_tools

WORK = ROOT / "build/hyfetch-qemu"


def prepare(cache):
    WORK.mkdir(parents=True, exist_ok=True)
    probe = WORK / "epoll-pty.elf"
    run([ROOT / "build/musl/sdk/bin/reliefos-musl-cc", "-static", "-O2", "-Wall", "-Wextra",
         ROOT / "kernel/reliefnt/tools/tests/linux_epoll_pty_test.c", "-o", probe])
    run([probe], timeout=10)
    with tempfile.TemporaryDirectory(prefix="stage-", dir=WORK) as directory:
        stage = Path(directory) / "root"
        make_live_tree(ROOT / "build/apk/root", stage)
        for home in ("root", "home/test"):
            config = stage / home / ".config/hyfetch.json"
            assert config.read_bytes() == (stage / "etc/skel/.config/hyfetch.json").read_bytes()
        assert (stage / "usr/share/fastfetch/leonos-ascii.txt").is_file()
        # Exercise the Live desktop path; PAM login is a separate regression.
        (stage / "etc/reliefos/installed").unlink()
        command = ["unshare", "-Ur", bootstrap(), "--root", stage]
        if cache:
            command += ["--no-network", "--cache-dir", cache.resolve()]
        run([*command, "add", "hyfetch@testing"], timeout=180)
        shutil.copy2(ROOT / "build/userland/terminal.elf",
                     stage / "usr/lib/reliefos/apps/terminal/terminal.elf")
        tests = stage / "usr/lib/reliefos/tests"
        tests.mkdir(parents=True, exist_ok=True)
        shutil.copy2(probe, tests / "linux-inventory.elf")
        (tests / "hyfetch.sh").write_text(
            "#!/bin/sh\nexport HOME=/root\n"
            "sudo hyfetch --config-file /tmp/hyfetch-wizard.json\nstatus=$?\n"
            "echo \"[hyfetch] first-status=$status\" > /dev/kmsg\n"
            "printf '[hyfetch] CONFIG %s\\n' \"$(tr -d '\\n' < /tmp/hyfetch-wizard.json)\" > /dev/kmsg\n"
            "printf '[hyfetch] DEFAULT %s\\n' \"$(tr -d '\\n' < /root/.config/hyfetch.json)\" > /dev/kmsg\n"
            "clear\n"
            "hyfetch\nstatus=$?\n"
            "echo \"[hyfetch] repeat-status=$status\" > /dev/kmsg\n",
            encoding="ascii")
        write_ext2_root(stage, WORK / "root.ext2", minimum_mib=512)
    iso_tools.GRUB_TEMPLATE = iso_tools.GRUB_TEMPLATE.replace(
        "autospawn=ioctlcloexec autospawn=python315", "autospawn=inventory").replace(
        "syscall-trace=/opt/python/", "").replace("set timeout=5", "set timeout=0")
    iso_tools.build_iso(WORK / "root.ext2", WORK / "hyfetch.iso", WORK / "grub.cfg", WORK)


def guest():
    import json
    import re
    from PIL import Image

    serial = WORK / "guest-serial.log"
    with tempfile.TemporaryDirectory(prefix="hyfetch-qmp-") as directory, (WORK / "qemu.log").open("w") as errors:
        qmp = Path(directory) / "qmp.sock"
        process = subprocess.Popen([
            "qemu-system-x86_64", "-enable-kvm", "-cpu", "host", "-machine", "q35", "-m", "4096",
            "-smp", "2,sockets=1,cores=2,threads=1", "-bios", "/usr/share/edk2/x64/OVMF.4m.fd",
            "-display", "none", "-serial", f"file:{serial}", "-device", "VGA,xres=1280,yres=720",
            "-netdev", "user,id=net0", "-device", "e1000,netdev=net0", "-cdrom", str(WORK / "hyfetch.iso"),
            "-boot", "d", "-qmp", f"unix:{qmp},server=on,wait=off", "-no-reboot", "-no-shutdown"],
            cwd=ROOT, stdout=subprocess.DEVNULL, stderr=errors)
        try:
            run(["python3", "tools/qmp_terminal_smoke.py", "--skip-oobe", "--hyfetch",
                 "--serial-log", serial, qmp], timeout=180)
        finally:
            if process.poll() is None:
                iso_tools.qmp_quit(qmp, process)
        text = serial.read_text(errors="replace")
        assert "[epoll-pty] DONE failures=0" in text, "raw epoll/PTYS probe failed"
        assert "[hyfetch] first-status=0" in text, "first-run sudo hyfetch failed"
        assert "[hyfetch] repeat-status=0" in text, "saved-config hyfetch failed"
        config = re.search(r"\[hyfetch\] CONFIG (\{[^\n]+\})", text)
        assert config, "configuration was not saved"
        value = json.loads(config[1])
        assert value["light_dark"] == "dark" and value["auto_detect_light_dark"] is True, value
        default = re.search(r"\[hyfetch\] DEFAULT (\{[^\n]+\})", text)
        assert default, "default ReliefOS HyFetch configuration was not installed"
        assert json.loads(default[1])["custom_ascii_path"] == "/usr/share/fastfetch/leonos-ascii.txt"
        with Image.open(ROOT / "build/images/hyfetch-qmp-smoke.ppm") as frame:
            frame.save(WORK / "terminal-hyfetch.png")
        print("PASS raw epoll/PTYS, sudo hyfetch wizard, ReliefOS identity and legacy logo alias")
        print(f"Evidence: {WORK}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package-cache", type=Path, help="use an existing signed APK index/package cache offline")
    parser.add_argument("--run-only", action="store_true")
    args = parser.parse_args()
    if not args.run_only:
        prepare(args.package_cache)
    guest()
