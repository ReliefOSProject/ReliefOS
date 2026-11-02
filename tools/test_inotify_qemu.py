#!/usr/bin/env python3
"""Check inotify ABI and Fileman refresh on a private ReliefOS disk copy.

Supply a matched kernel/root/ESP bundle produced by reliefos-kernel-tools, a
static native probe, and the Fileman app build. Existing images are read-only.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time
import uuid

from PIL import ImageOps

import test_vdso_qemu as disk
import test_xorg_vm_qemu as xorg
from test_fltk_apps_qemu import target_compile_probe
from run_gcc_probe_qemu import qmp_quit

ROOT = Path(__file__).resolve().parents[1]
GUEST = "/usr/lib/reliefos/tests/inotify"
APP = "/usr/lib/reliefos/apps/fileman/fileman.elf"
DIRECTORY = "/home/test/inotify-ui"


def run(command, **kwargs):
    return subprocess.run(list(map(str, command)), cwd=ROOT, check=True, **kwargs)


def prepare(args, output):
    source_hash = disk.file_sha256(args.image)
    raw = output / "guest.raw"
    run(["cp", "--reflink=auto", "--sparse=always", args.image, raw])
    parts = disk.gpt_partitions(raw)
    root = next(p for p in parts if p["name"] == "RELIEFOS_ROOT")
    esp = next(p for p in parts if p["name"] == "RELIEFOS_ESP")
    layout = {"root_first": root["first"], "root_last": root["last"],
              "esp_first": esp["first"]}
    filesystem = disk.extract_root(raw, layout, output)
    nonce = uuid.uuid4().hex
    runner = output / "runner"
    runner.write_text(f'''#!/bin/sh
exec >/dev/ttyS0 2>&1
echo "INOTIFY_RUN_START nonce={nonce} release=$(uname -r)"
status=0
control=/proc/sys/fs/inotify/max_user_instances
old=$(cat "$control") || status=1
cat /proc/sys/fs/inotify/* || status=1
printf '32\\n' >"$control" || status=1
test "$(cat "$control")" = 32 || status=1
if printf '%s\\n' -1 >"$control"; then status=1; fi
test "$(cat "$control")" = 32 || status=1
sysctl -w "fs.inotify.max_user_instances=$old" || status=1
{GUEST}/probe /root/inotify-{nonce} || status=1
{GUEST}/probe /dev/shm/inotify-{nonce} || status=1
echo "INOTIFY_RUN_DONE nonce={nonce} status=$status"
''')
    service = output / "service"
    service.write_text(f'''#!/sbin/openrc-run
description="Private inotify ABI checks"
depend() {{ need reliefos-runtime; after localmount; before xdm reliefos-session; }}
start() {{ /bin/sh {GUEST}/runner; return 0; }}
''')
    geometry = target_compile_probe(output, args.app_build / "sysroot/musl", "clang++",
                                    "ld.lld", build_dir=args.app_build)
    # Give the owned QA terminal a ready marker and a plain shell. A user's
    # login profile (e.g. fastfetch) must not consume the scripted input before
    # a command interpreter exists. The production image/script stays intact.
    session = output / "Xsession"
    run(["debugfs", "-R", f'dump /usr/lib/X11/xdm/Xsession "{session}"', filesystem],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    session_text = session.read_text()
    shell_command = '"${SHELL:-/bin/sh}" -l'
    if session_text.count(shell_command) != 1:
        raise RuntimeError("QA terminal could not identify the login-shell command")
    session.write_text(session_text.replace(shell_command, f"/bin/sh {GUEST}/terminal"))
    terminal = output / "terminal"
    terminal.write_text(f"#!/bin/sh\nexport ENV={GUEST}/shellrc\nexec /bin/sh -i\n")
    shellrc = output / "shellrc"
    shellrc.write_text(f'''echo INOTIFY_SHELL_READY >/dev/ttyS0
/bin/sh {GUEST}/gui
echo INOTIFY_QA_READY >/dev/ttyS0
/bin/sh {GUEST}/mutations &
''')
    mutations = output / "mutations"
    # Each phase remains visible long enough to capture pixels and OCR. Changes
    # originate in a separate guest process; no keyboard or refresh action is
    # involved after login, isolating inotify from QMP focus/input delivery.
    mutations.write_text(f'''#!/bin/sh
for step in 1 2 3; do
    sleep 20
    /bin/sh {GUEST}/change "$step"
done
''')
    gui = output / "gui"
    gui.write_text(f'''#!/bin/sh
exec >/dev/ttyS0 2>&1
if printf '1\\n' >/proc/sys/fs/inotify/max_user_instances; then
    echo INOTIFY_GUI_DAC_FAILED
    exit 1
fi
export LC_ALL=C.UTF-8 LANG=C.UTF-8
{APP} {DIRECTORY} &
pid=$!
echo "INOTIFY_GUI_START pid=$pid"
{GUEST}/geometry --window-info "$pid"
''')
    change = output / "change"
    change.write_text(f'''#!/bin/sh
case "$1" in
1) touch {DIRECTORY}/created-marker.txt;;
2) mv {DIRECTORY}/created-marker.txt {DIRECTORY}/renamed-marker.txt;;
3) rm {DIRECTORY}/renamed-marker.txt;;
*) exit 64;;
esac
echo "INOTIFY_GUI_CHANGE step=$1 status=$?" >/dev/ttyS0
''')
    before = output / "before.txt"
    before.write_text("Initial Fileman listing.\n")
    payload = [(path, "/" + path.relative_to(args.bundle / "root-payload").as_posix(),
                path.stat().st_mode & 0o7777)
               for path in sorted((args.bundle / "root-payload").rglob("*")) if path.is_file()]
    payload += [(args.probe, GUEST + "/probe", 0o755),
                (runner, GUEST + "/runner", 0o755),
                (geometry, GUEST + "/geometry", 0o755),
                (session, "/usr/lib/X11/xdm/Xsession", 0o755),
                (terminal, GUEST + "/terminal", 0o755),
                (shellrc, GUEST + "/shellrc", 0o644),
                (mutations, GUEST + "/mutations", 0o755),
                (gui, GUEST + "/gui", 0o755), (change, GUEST + "/change", 0o755),
                (service, "/etc/init.d/reliefos-inotify-qa", 0o755),
                (args.app_build / "userland/fileman.elf", APP, 0o755),
                (before, DIRECTORY + "/before.txt", 0o644)]
    commands, made = [], set()

    def directory(path):
        if path in made or disk.debugfs_exists(filesystem, path):
            return
        directory(str(Path(path).parent))
        commands.append(f'mkdir "{path}"')
        made.add(path)

    pins = {}
    for source, destination, mode in payload:
        directory(str(Path(destination).parent))
        if disk.debugfs_exists(filesystem, destination):
            commands.append(f'rm "{destination}"')
        commands += [f'write "{source}" "{destination}"',
                     f'set_inode_field "{destination}" mode 0{0o100000 | mode:o}']
    commands += [f'set_inode_field {DIRECTORY} uid 1000',
                 f'set_inode_field {DIRECTORY} gid 1000']
    link = "/etc/runlevels/default/reliefos-inotify-qa"
    if disk.debugfs_exists(filesystem, link):
        commands.append("rm " + link)
    commands.append("symlink " + link + " /etc/init.d/reliefos-inotify-qa")
    batch = output / "inject.commands"
    batch.write_text("\n".join(commands) + "\n")
    with (output / "inject.log").open("w") as log:
        run(["debugfs", "-w", "-f", batch, filesystem], stdout=log, stderr=log)
    back = output / "readbacks"
    back.mkdir()
    for i, (source, destination, mode) in enumerate(payload):
        target = back / str(i)
        run(["debugfs", "-R", f'dump "{destination}" "{target}"', filesystem],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        pins[destination] = disk.file_sha256(source)
        if disk.file_sha256(target) != pins[destination]:
            raise RuntimeError("root readback differs: " + destination)
        stat = run(["debugfs", "-R", f'stat "{destination}"', filesystem],
                   capture_output=True, text=True).stdout
        found_mode = re.search(r"Mode:\s+([0-7]+)", stat)
        if not found_mode or int(found_mode[1], 8) != mode:
            raise RuntimeError("root payload mode differs: " + destination)
    run(["e2fsck", "-fn", filesystem], stdout=(output / "e2fsck.log").open("w"),
        stderr=subprocess.STDOUT)
    disk.put_root_back(raw, filesystem, layout)
    image_arg = f"{raw}@@{int(esp['first']) * 512}"
    tree = args.bundle / "esp-payload"
    for path in sorted(tree.rglob("*")):
        target = "::/" + path.relative_to(tree).as_posix()
        if path.is_dir():
            subprocess.run(["mmd", "-i", image_arg, target], capture_output=True, check=False)
            continue
        run(["mcopy", "-o", "-i", image_arg, path, target])
        readback = back / ("esp-" + path.relative_to(tree).as_posix().replace("/", "_"))
        run(["mcopy", "-i", image_arg, target, readback])
        pins[target] = disk.file_sha256(path)
        if disk.file_sha256(readback) != pins[target]:
            raise RuntimeError("ESP readback differs: " + target)
    if disk.file_sha256(args.image) != source_hash:
        raise RuntimeError("source disk changed during staging")
    release = json.loads((args.bundle / "install/install.bundle.json").read_text())["release"]
    return raw, {"source": str(args.image), "source_sha256": source_hash,
                 "source_unchanged": True, "release": release, "pins": pins,
                 "nonce": nonce, "vmware_verified": False, "passed": False,
                 "qa_terminal_plain_shell": True}


def wait_ocr(probe, process, output, geometry, tag, present, absent=()):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("QEMU exited during Fileman checks")
        frame = probe.frame(tag)
        x, y, width, height = geometry
        crop = output / (tag + ".client.png")
        frame.crop((x, y, x + width, y + height)).save(crop)
        text = subprocess.check_output(["tesseract", str(crop), "stdout", "-l", "eng",
                                        "--psm", "11"], stderr=subprocess.DEVNULL, text=True)
        # Tesseract's sparse-page mode misses white names on selected blue rows.
        # Normalize only the list's selection background for a second OCR pass;
        # retain the untouched whole-desktop and client screenshots as evidence.
        listing = frame.crop((x + 214, y + 138, x + width - 150,
                              y + height - 60)).convert("RGB")
        for row in range(listing.height):
            red, green, blue = listing.getpixel((listing.width - 2, row))
            if blue > red + 32 and blue > green + 32:
                band = listing.crop((0, row, listing.width, row + 1))
                listing.paste(ImageOps.invert(band), (0, row))
        listing = listing.convert("L").point(lambda value: 255 if value > 150 else 0)
        listing = ImageOps.expand(listing, border=8, fill=255)
        listing_path = output / (tag + ".list-ocr.png")
        listing.resize((listing.width * 2, listing.height * 2)).save(listing_path)
        text += subprocess.check_output(["tesseract", str(listing_path), "stdout", "-l", "eng",
                                         "--psm", "6"], stderr=subprocess.DEVNULL, text=True)
        (output / (tag + ".txt")).write_text(text)
        normalized = re.sub(r"[^a-z0-9]", "", text.lower())
        if all(word in normalized for word in present) and not any(word in normalized for word in absent):
            return
        time.sleep(0.3)
    raise AssertionError(f"Fileman {tag}: expected {present}, absent {absent}; OCR={text!r}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True, help="raw GPT input disk")
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--app-build", type=Path, required=True)
    parser.add_argument("--probe", type=Path, required=True, help="static native ABI probe")
    parser.add_argument("--output", type=Path, required=True, help="new evidence directory")
    args = parser.parse_args()
    for name in ("image", "bundle", "app_build", "probe", "output"):
        setattr(args, name, getattr(args, name).resolve())
    args.output.mkdir(parents=True, exist_ok=False)
    temporary = ROOT / "out/tmp"
    temporary.mkdir(parents=True, exist_ok=True)
    os.environ["TMPDIR"] = tempfile.tempdir = str(temporary)
    raw, record = prepare(args, args.output)
    runtime = Path(tempfile.mkdtemp(prefix="inotify-", dir=temporary))
    socket_path = runtime / "qmp.sock"
    if len(str(socket_path).encode()) >= 108:
        raise RuntimeError("QMP socket path too long")
    process, serial, qmp = xorg.boot([], raw, runtime)
    probe = None
    try:
        xorg.wait_log(serial, "INOTIFY_RUN_DONE", process, timeout=180)
        log = serial.read_text(errors="replace")
        marker = f"INOTIFY_RUN_DONE nonce={record['nonce']} status=0"
        if marker not in log or log.count("[inotify] PASS checks=") != 2:
            raise AssertionError("native inotify probe failed; see serial.log")
        if f"release={record['release']}" not in log:
            raise AssertionError("guest did not boot the staged kernel")
        probe = xorg.Probe(qmp, args.output)
        xorg.greeter_login(probe, process, timeout=120)
        xorg.wait_session(probe, process, "inotify-session", timeout=60)
        xorg.wait_log(serial, "INOTIFY_QA_READY", process, timeout=90)
        recent = serial.read_text(errors="replace")
        match = re.search(r"FLTK_APP_GEOMETRY pid=\d+ x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)", recent)
        if not match or "INOTIFY_GUI_DAC_FAILED" in recent:
            raise AssertionError("Fileman geometry or sysctl DAC check failed")
        geometry = tuple(map(int, match.groups()))
        # Match the unique stems: the small bitmap font's .txt suffix is often
        # read as .lxl, while the event probe separately checks exact name bytes.
        wait_ocr(probe, process, args.output, geometry, "fileman-before", ["before"],
                 ["createdmarker", "renamedmarker"])
        for step, present, absent in ((1, ["createdmarker"], []),
                                      (2, ["renamedmarker"], ["createdmarker"]),
                                      (3, ["before"], ["renamedmarker", "createdmarker"])):
            xorg.wait_log(serial, f"INOTIFY_GUI_CHANGE step={step} status=0", process, timeout=35)
            wait_ocr(probe, process, args.output, geometry, f"fileman-step-{step}", present, absent)
        record["passed"] = True
        record["fileman_geometry"] = geometry
    except Exception as error:
        record["error"] = repr(error)
        if probe is not None:
            probe.frame("failure")
        raise
    finally:
        shutil.copyfile(serial, args.output / "serial.log")
        shutil.copyfile(runtime / "qemu.log", args.output / "qemu.log")
        if probe is not None:
            probe.stream.close()
            probe.socket.close()
        qmp_quit(qmp, process)
        (args.output / "result.json").write_text(json.dumps(record, indent=2) + "\n")
    print("PASS: ext4/tmpfs inotify and Fileman create/rename/delete refresh", flush=True)


if __name__ == "__main__":
    main()
