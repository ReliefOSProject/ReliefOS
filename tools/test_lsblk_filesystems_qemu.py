#!/usr/bin/env python3
"""Check the production lsblk against real GPT/FAT/ext metadata in a scratch guest."""
import argparse
import json
from pathlib import Path
import subprocess
import time

from test_installer_accounts_qemu import boot

ROOT = Path(__file__).resolve().parents[1]


def run(*args, **kwargs):
    return subprocess.run(list(map(str, args)), cwd=ROOT, check=True, **kwargs)


def partition_file(disk, partition, output):
    run("dd", f"if={disk}", f"of={output}", "bs=512", f"skip={partition['start']}",
        f"count={partition['size']}", "status=none")


def filesystem(path):
    text = subprocess.check_output(["blkid", "-p", "-o", "export", str(path)], text=True)
    return dict(line.split("=", 1) for line in text.splitlines() if "=" in line)


def frame_until(probe, process, name, predicate):
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        assert process.poll() is None, "guest exited"
        frame = probe.frame(name)
        if predicate(frame):
            return
        time.sleep(.5)
    raise AssertionError("guest screen not ready: " + name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True, help="fresh production raw disk")
    parser.add_argument("--smp", type=int, choices=(1, 2), default=2)
    parser.add_argument("--allow-empty-fsver", action="store_true",
                        help="accept the documented official Alpine lsblk 2.42.4-r3 probe limitation")
    args = parser.parse_args()
    output = ROOT / f"build/lsblk-filesystems-qemu/{args.smp}cpu"
    output.mkdir(parents=True, exist_ok=True)
    disk = output / "scratch.raw"
    run("cp", "--reflink=auto", args.image.resolve(), disk)
    partitions = json.loads(subprocess.check_output(["sfdisk", "--json", str(disk)]))["partitiontable"]["partitions"]
    assert len(partitions) == 2, "expected the production ESP/root disk layout"
    esp, root = output / "esp.fat", output / "root.ext4"
    partition_file(disk, partitions[0], esp)
    partition_file(disk, partitions[1], root)
    expected = [filesystem(esp), filesystem(root)]

    binary = output / "block-metadata.elf"
    run("clang", "--target=x86_64-linux-musl",
        f"--sysroot={ROOT / 'out/x86_64/release/sysroot/musl'}", "-fuse-ld=lld", "-static",
        "-O2", "-Wall", "-Wextra", ROOT / "tools/tests/block_metadata_guest.c", "-o", binary)
    script = output / "verify.sh"
    script.write_text("""#!/bin/sh
exec >/root/lsblk-verify.log 2>&1
if ! /usr/lib/reliefos/tests/block-metadata.elf; then
    sync
    printf '[lsblk-probe] DONE\\n' >/dev/serial0
    exit 1
fi
printf 'LSBLK_JSON_BEGIN\\n'
lsblk --json --output NAME,FSTYPE,FSVER,LABEL,UUID,PARTUUID,FSAVAIL,FSUSE%,MOUNTPOINTS
printf 'LSBLK_JSON_END\\n'
lsblk -f
sync
printf '[lsblk-probe] DONE\\n' >/dev/serial0
""")
    target = "/usr/lib/reliefos/tests/block-metadata.elf"
    for command in ("mkdir /usr/lib/reliefos/tests", f"rm {target}", f"write {binary} {target}",
                    f"set_inode_field {target} mode 0100755", f"write {script} /root/lsblk-verify.sh"):
        run("debugfs", "-w", "-R", command, root, capture_output=True)
    embedded = subprocess.check_output(["debugfs", "-R", f"cat {target}", str(root)], stderr=subprocess.DEVNULL)
    assert embedded == binary.read_bytes(), "guest regression binary was not embedded"
    run("dd", f"if={root}", f"of={disk}", "bs=512", f"seek={partitions[1]['start']}",
        "conv=notrunc", "status=none")
    with boot(output, disk, smp=args.smp) as (probe, serial, process):
        frame_until(probe, process, "login", lambda f: f.width == 1920 and f.getpixel((705, 365)) == (229, 229, 229))
        probe.click(800, 482)
        probe.click(900, 604)
        probe.text("root")
        probe.click(1140, 678)
        frame_until(probe, process, "desktop", lambda f: f.getpixel((705, 365)) != (229, 229, 229) and
                    f.getpixel((20, 1060)) != (0, 120, 212))
        probe.click(45, 1064)
        probe.click(225, 748)
        time.sleep(2)
        probe.text("sh /root/lsblk-verify.sh")
        probe.key("ret")
        deadline = time.monotonic() + 120
        while "[lsblk-probe] DONE" not in serial.read_text(errors="replace"):
            assert process.poll() is None and time.monotonic() < deadline, "guest regression failed or timed out"
            time.sleep(.5)
        probe.text("lsblk -f")
        probe.key("ret")
        time.sleep(5)
        probe.frame("lsblk-filesystems")
    partition_file(disk, partitions[1], root)
    log = subprocess.check_output(["debugfs", "-R", "cat /root/lsblk-verify.log", str(root)], stderr=subprocess.DEVNULL).decode()
    (output / "verify.log").write_text(log)
    assert "PASS block metadata" in log, log
    data = json.loads(log.split("LSBLK_JSON_BEGIN\n", 1)[1].split("LSBLK_JSON_END", 1)[0])
    devices = {item["name"]: item for item in data["blockdevices"][0]["children"]}
    for i, name in enumerate(("sda1", "sda2")):
        device, metadata = devices[name], expected[i]
        for column, key in (("fstype", "TYPE"), ("fsver", "VERSION"), ("label", "LABEL"), ("uuid", "UUID")):
            if column == "fsver" and args.allow_empty_fsver and not device[column]:
                continue
            assert (device[column] or "") == metadata.get(key, ""), (name, column, device, metadata)
        assert device["partuuid"].lower() == partitions[i]["uuid"].lower(), device
        assert device["mountpoints"] == [["/boot", "/"][i]], device
        assert device["fsavail"] and device["fsuse%"].endswith("%"), device
    version = " (empty FSVER permitted for the official APK)" if args.allow_empty_fsver else ""
    print(f"PASS QEMU {args.smp} CPU: real filesystem metadata, usage and block-read DAC{version}")


if __name__ == "__main__":
    main()
