#!/usr/bin/env python3
"""Cold-boot an installed AHCI root and check first-submit GUI account switching."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time

from test_installer_accounts_qemu import boot, install_tty, wait_log, USER_PASSWORD, ROOT_PASSWORD
from test_pam_desktop_qemu import root_partition
from test_sudo_e2e_qemu import Probe

ROOT = Path(__file__).resolve().parents[1]


def patch_kernel(disk, kernel):
    table = json.loads(subprocess.check_output(["sfdisk", "--json", str(disk)]))["partitiontable"]
    offset = table["partitions"][0]["start"] * table["sectorsize"]
    image = f"{disk}@@{offset}"
    for namespace in ("reliefos", "leonos"):
        subprocess.run(["mcopy", "-o", "-i", image, str(kernel), f"::/{namespace}/kernel.sys"], check=True)
    embedded = subprocess.check_output(["mtype", "-i", image, "::/leonos/kernel.sys"])
    assert embedded == kernel.read_bytes(), "scratch ESP kernel does not match the tested build"
    return hashlib.sha256(embedded).hexdigest()


def login_case(output, disk, wrong_password=False):
    # One vCPU also covers serialized AHCI continuation without another CPU.
    with boot(output, disk, smp=1) as (probe, serial, process):
        probe.__class__ = Probe
        wait_log(serial, "[login.elf] starting login UI", process, timeout=60)
        time.sleep(3)
        frame = probe.frame("login")
        x, y = (frame.width - 520) // 2 + 80, (frame.height - 360) // 2 + 112
        probe.click(x, y + 10)
        probe.text(ROOT_PASSWORD)
        probe.frame("root-unsubmitted")
        probe.click(x, y + 30)
        probe.frame("ordinary-account")
        assert "[pam-login]" not in serial.read_text(errors="replace"), "switching submitted authentication"
        if wrong_password:
            probe.text("wrong-password")
            probe.key("ret")
            wait_log(serial, "[pam-login] stage=authenticate status=7", process, timeout=30)
            assert "[pam-login] session ready" not in serial.read_text(errors="replace")
            time.sleep(.5)
            probe.frame("wrong-password-rejected")
        probe.text(USER_PASSWORD)
        probe.key("ret")
        # There is deliberately no retry: the first correct submission must work.
        wait_log(serial, "[pam-login] session ready", process, timeout=30)
        log = serial.read_text(errors="replace")
        assert log.count("[pam-login] authentication accepted") == 1, log
        assert log.count("[pam-login] session ready") == 1, log
        assert "lazy file map failed" not in log, log
        failures = [line for line in log.splitlines() if "[pam-login] stage=" in line]
        assert len(failures) == int(wrong_password), failures
        time.sleep(3)
        probe.frame("first-submit-desktop")
        probe.key("meta_l")
        time.sleep(1)
        probe.text("terminal")
        probe.key("ret")
        time.sleep(3)
        probe.text("echo PAM_SWITCH_ST_READY >/dev/ttyS0")
        probe.key("ret")
        wait_log(serial, "PAM_SWITCH_ST_READY", process, timeout=30)
        command = ("grep -E '^(Uid|Gid):' /proc/self/status > /tmp/login-switch-identity; "
                   "printf 'Groups: ' >> /tmp/login-switch-identity; "
                   "id -G >> /tmp/login-switch-identity; "
                   "pwd >> /tmp/login-switch-identity; cat /tmp/login-switch-identity; sync")
        probe.text(command)
        probe.key("ret")
        wait_log(serial, "name=sync code=0", process, timeout=30)
        time.sleep(1)
        probe.frame("terminal-identity")
    filesystem = output / "root-check.ext2"
    try:
        root_partition(disk, filesystem)
        identity = subprocess.check_output(
            ["debugfs", "-R", "cat /tmp/login-switch-identity", str(filesystem)],
            text=True, stderr=subprocess.DEVNULL)
        (output / "identity.txt").write_text(identity)
        assert "Uid:\t1000\t1000\t1000\t1000" in identity, identity
        assert "Gid:\t1000\t1000\t1000\t1000" in identity, identity
        groups = next(line for line in identity.splitlines() if line.startswith("Groups:")).split()[1:]
        assert "10" in groups and "0" not in groups, identity
        assert "/home/alice" in identity, identity
    finally:
        filesystem.unlink(missing_ok=True)
    return {"case": output.name, "first_correct_submission": True,
            "wrong_password_rejected": wrong_password, "identity": identity,
            "serial": str(serial)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", type=Path, default=ROOT / "build/images/reliefos-installer.iso")
    parser.add_argument("--kernel", type=Path, default=ROOT / "build/system/kernel.sys")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists() or not output.is_relative_to(ROOT / "build"):
        parser.error("output must be a new directory below build")
    output.mkdir(parents=True)
    disk = output / "scratch.raw"
    with disk.open("wb") as stream:
        stream.truncate(4 * 1024**3)
    with boot(output / "install", disk, args.iso.resolve(), smp=1) as session:
        install_tty(*session, "none")
    result = {"kernel_sha256": patch_kernel(disk, args.kernel.resolve()), "cases": [],
              "qemu": "q35/KVM, 1 vCPU, AHCI installed ext2 root", "complete": False}
    try:
        for name, wrong in (("cold-switch-1", False), ("cold-switch-2", False),
                            ("cold-switch-wrong-password", True)):
            result["cases"].append(login_case(output / name, disk, wrong))
            print(f"PASS {name}: first correct submission, ordinary uid and wheel groups", flush=True)
        result["complete"] = True
    finally:
        (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f"PASS installed desktop account-switch regression: {output}")


if __name__ == "__main__":
    main()
