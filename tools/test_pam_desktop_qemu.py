#!/usr/bin/env python3
"""Exercise PAM login and Terminal startup on a disposable copy of a raw disk."""
import argparse
import json
import re
from pathlib import Path
import subprocess
import time

from test_installer_accounts_qemu import boot, wait_log, install_tty, USER_PASSWORD
from test_sudo_e2e_qemu import Probe

ROOT = Path(__file__).resolve().parents[1]


def root_partition(disk, filesystem):
    table = json.loads(subprocess.check_output(["sfdisk", "--json", str(disk)]))["partitiontable"]
    root = table["partitions"][1]
    start, size = root["start"] * table["sectorsize"], root["size"] * table["sectorsize"]
    subprocess.run(["dd", f"if={disk}", f"of={filesystem}", "bs=4M",
                    f"skip={start}B", f"count={size}B", "conv=sparse", "status=none"], check=True)
    return start


def prepare_probe(disk, output, core=False):
    recipe = json.loads((ROOT / "build/auth-upstream/linux-pam-build.json").read_text())
    name = "core-probe" if core else "flock-probe"
    source = "core_limit_runtime_probe.c" if core else "flock_nonblock_runtime_probe.c"
    binary = output / name
    subprocess.run([*recipe["compiler"], "-static", "-O2",
                    "-pthread", str(ROOT / "kernel/reliefnt/tools/tests" / source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
    filesystem = output / "root-check.ext2"
    start = root_partition(disk, filesystem)
    # Only the scratch image is changed; the regression is not shipped in ISO.
    for command in (f"rm /bin/{name}", f'write "{binary}" /bin/{name}',
                    f"set_inode_field /bin/{name} mode 0100755",
                    "rm /tmp/pam-uid", "rm /tmp/flock-result", "rm /tmp/core-sudo"):
        subprocess.run(["debugfs", "-w", "-R", command, str(filesystem)], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["dd", f"if={filesystem}", f"of={disk}", "bs=4M",
                    f"seek={start}B", "conv=notrunc", "status=none"], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--disk", type=Path, help="standalone raw image with test/test account")
    source.add_argument("--iso", type=Path, help="install to a new scratch disk before testing")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--core", action="store_true", help="verify raw RLIMIT_CORE and upstream sudo su")
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists() or not output.is_relative_to(ROOT / "build"):
        parser.error("output must be a new directory below build")
    output.mkdir(parents=True)
    disk = output / "scratch.raw"
    password = "test"
    if args.iso:
        with disk.open("wb") as stream:
            stream.truncate(4 * 1024**3)
        with boot(output / "install", disk, args.iso.resolve()) as session:
            install_tty(*session, "none")
        password = USER_PASSWORD
    else:
        subprocess.run(["cp", "--reflink=auto", str(args.disk.resolve()), str(disk)], check=True)
    prepare_probe(disk, output, args.core)
    with boot(output, disk) as (probe, serial, process):
        probe.__class__ = Probe
        try:
            wait_log(serial, "[login.elf] starting login UI", process, timeout=60)
            time.sleep(3)
            probe.frame("login")
            probe.key("down")
            probe.text(password)
            probe.key("ret")
            wait_log(serial, "[pam-login] session ready", process, timeout=30)
            time.sleep(3)
            probe.frame("after-login")
            probe.key("meta_l")
            time.sleep(1)
            probe.text("terminal")
            probe.key("ret")
            wait_log(serial, "terminal: PTY ready", process, timeout=30)
            time.sleep(4)
            name = "core-probe" if args.core else "flock-probe"
            probe.text(f"grep Uid /proc/self/status > /tmp/pam-uid; /bin/{name} > /tmp/flock-result 2>&1; echo PAM_DESKTOP_OK")
            probe.key("ret")
            time.sleep(5)
            probe.frame("terminal-identity")
            probe.text("cat /tmp/pam-uid /tmp/flock-result; sync")
            probe.key("ret")
            wait_log(serial, "name=sync code=0", process, timeout=30)
            time.sleep(1)
            probe.frame("terminal-results")
            if args.core:
                sudo_offset = len(serial.read_text(errors="replace"))
                probe.text("sudo -k; sudo -p CORE_PASSWORD /bin/su -c '/bin/core-probe --sudo-child' > /tmp/core-sudo 2>&1; echo CORE_EXIT=$? >> /tmp/core-sudo")
                probe.key("ret")
                # The prompt is redirected to the guest file; allow sudo to open /dev/tty.
                time.sleep(4)
                probe.text(password)
                probe.key("ret")
                deadline = time.monotonic() + 45
                while True:
                    log = serial.read_text(errors="replace")[sudo_offset:]
                    parents = re.findall(r"exec pid=(\d+) path=/usr/bin/sudo ", log)
                    if len(parents) >= 2 and re.search(
                            rf"scheduler wait reaped pid={parents[1]} by pid=\d+", log):
                        break
                    if process.poll() is not None or time.monotonic() >= deadline:
                        raise AssertionError("sudo did not return to its calling shell")
                    time.sleep(.2)
                time.sleep(1)
                sync_count = serial.read_text(errors="replace").count("name=sync code=0")
                probe.text("cat /tmp/core-sudo; sync")
                probe.key("ret")
                deadline = time.monotonic() + 30
                while serial.read_text(errors="replace").count("name=sync code=0") <= sync_count:
                    if process.poll() is not None or time.monotonic() >= deadline:
                        raise AssertionError("sudo result sync did not complete")
                    time.sleep(.2)
                time.sleep(1)
                probe.frame("sudo-core-results")
        except Exception:
            samples = []
            for _ in range(3):
                samples.append(probe.command("human-monitor-command", {"command-line": "info registers"}))
                time.sleep(.3)
            (output / "cpu-samples.json").write_text(json.dumps(samples, indent=2))
            raise
    filesystem = output / "root-check.ext2"
    root_partition(disk, filesystem)
    results = [("/tmp/pam-uid", "Uid:\t1000\t1000\t1000\t1000"),
               ("/tmp/flock-result", f"[{'core-limit' if args.core else 'flock-nb'}] DONE failures=0")]
    if args.core:
        results.append(("/tmp/core-sudo", "CORE_SUDO_OK uid=0 core=0,0"))
    for path, expected in results:
        result = subprocess.check_output(["debugfs", "-R", f"cat {path}", str(filesystem)], text=True)
        (output / Path(path).name).write_text(result)
        if expected not in result:
            raise AssertionError(f"guest result missing {expected!r}: {result!r}")
        if "Function not implemented" in result or "setrlimit(" in result:
            raise AssertionError(f"resource-limit warning in {path}: {result!r}")
        if path == "/tmp/core-sudo" and "CORE_EXIT=0" not in result:
            raise AssertionError(f"sudo su failed: {result!r}")
    print(f"PASS desktop login, Terminal uid=1000, {'CORE and sudo su' if args.core else 'flock'} regression: {output}")


if __name__ == "__main__":
    main()
