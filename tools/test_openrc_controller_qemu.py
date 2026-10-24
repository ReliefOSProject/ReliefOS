#!/usr/bin/env python3
"""Accept the shipped OpenRC Controller on a private ReliefOS image only."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time
from PIL import ImageChops
import test_vdso_qemu as disk
import test_xorg_vm_qemu as xorg
from test_installer_accounts_qemu import Probe
from run_gcc_probe_qemu import qmp_quit

ROOT = Path(__file__).resolve().parents[1]
GUEST = "/usr/lib/reliefos/tests/openrc-controller"
APP = "/usr/lib/reliefos/apps/openrc-controller/openrc-controller.elf"


def login(probe, process):
    # The stock XDM panel stays about 640 px wide even on a 1920 px desktop;
    # find its neutral gray background instead of assuming a 1280 px screen.
    deadline = time.monotonic() + 180
    box = None
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("QEMU exited before login")
        frame = probe.frame("greeter")
        channels = frame.convert("RGB").split()
        masks = [channel.point(lambda value: 255 if 185 <= value <= 195 else 0) for channel in channels]
        mask = ImageChops.multiply(ImageChops.multiply(masks[0], masks[1]), masks[2])
        candidate = mask.getbbox()
        if candidate and mask.histogram()[255] > 50000 and candidate[2] - candidate[0] >= 400:
            box = candidate
            break
        time.sleep(.5)
    if box is None:
        raise AssertionError("XDM login panel did not appear")
    probe.click(box[0] + 320, box[1] + 117)
    probe.text("test")
    probe.key("ret")
    time.sleep(1)
    probe.text("test")
    probe.key("ret")
    xorg.wait_session(probe, process, "desktop-ready", timeout=120)


def run(args, **kwargs):
    return subprocess.run(list(map(str, args)), cwd=ROOT, check=True,
                          env={**os.environ, "TMPDIR": str(ROOT / "out/tmp")}, **kwargs)


def digest(path):
    with path.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def prepare(build, output):
    output.mkdir(parents=True, exist_ok=False)
    image = build / "images/reliefos.vmdk"
    evidence = {"source_image": str(image), "source_sha256": digest(image), "checks": []}
    raw, layout = disk.private_raw_image(image, output)
    root = disk.extract_root(raw, layout, output)
    commands = []

    def put(source, target, mode="0100644"):
        disk.add_missing_root_directory(root, str(Path(target).parent), commands)
        if disk.debugfs_exists(root, target):
            commands.append(f"rm {target}")
        commands.extend([f'write "{source}" {target}', f"set_inode_field {target} mode {mode}"])

    def text(name, content, target, mode="0100644"):
        source = output / name
        source.write_text(content)
        put(source, target, mode)

    def read(path, target):
        run(["debugfs", "-R", f'dump {path} "{target}"', root], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    shipped = output / "shipped-app.elf"
    read(APP, shipped)
    assert digest(shipped) == digest(build / "userland/openrc-controller.elf")
    evidence["app_sha256"] = digest(shipped)
    kernel = output / "shipped-kernel.sys"
    run(["mcopy", "-i", f'{raw}@@{int(layout["esp_first"]) * 512}', "::/reliefos/kernel.sys", kernel])
    assert digest(kernel) == digest(build / "generated/system/kernel.sys")
    evidence["kernel_sha256"] = digest(kernel)
    compiler = build / "sdk/reliefos-musl-sdk/bin/reliefos-musl-cc"
    common = [compiler, "-O2", "-Wall", "-Wextra"]
    run([*common, "-x", "c++", "-std=c++17", "-nostdinc++", "-fno-exceptions", "-fno-rtti",
         "-I" + str(ROOT / "userland/apps/openrc-controller"),
         ROOT / "tools/tests/openrc_controller_guest.cpp", ROOT / "userland/apps/openrc-controller/model.cpp",
         "-o", output / "guest-probe"])
    run([*common, "-isystem", build / "upstream/x11-development/usr/include",
         ROOT / "tools/tests/openrc_controller_window_probe.c",
         "-L" + str(build / "upstream/x11-development/usr/lib"), "-lX11", "-o", output / "window-probe"])
    put(output / "guest-probe", GUEST + "/guest-probe", "0100755")
    put(output / "window-probe", GUEST + "/window-probe", "0100755")
    for database in ("passwd", "shadow", "group", "gshadow"):
        source = output / ("private-" + database)
        read("/etc/" + database, source)
        lines = [line for line in source.read_text().splitlines() if not line.startswith("test:")]
        fixture = (ROOT / "system/test-accounts" / database).read_text().splitlines()
        lines.extend(line for line in fixture if line.startswith("test:"))
        if database == "shadow":
            hashed = run(["openssl", "passwd", "-6", "-stdin"], input="controller-root\n", text=True, capture_output=True).stdout.strip()
            lines = [":".join(["root", hashed, *line.split(":")[2:]]) if line.startswith("root:") else line for line in lines]
        if database == "group":
            lines = [line if not line.startswith("wheel:") else ":".join([*line.split(":")[:3], ",".join(sorted(set(filter(None, line.split(":")[3].split(","))) | {"test"}))]) for line in lines]
        source.write_text("\n".join(lines) + "\n")
        source.chmod(0o600)
        put(source, "/etc/" + database, "0100600" if "shadow" in database else "0100644")
    disk.add_missing_root_directory(root, "/home/test", commands)
    commands.extend(["set_inode_field /home/test uid 1000", "set_inode_field /home/test gid 1000", "set_inode_field /home/test mode 040700"])
    text("controller-qa", '''#!/sbin/openrc-run
description="Controller acceptance service"
extra_commands="health"
extra_started_commands="reload"
start() { echo started > /run/controller-qa; }
stop() { rm -f /run/controller-qa; }
reload() { echo reloaded > /run/controller-qa; }
health() { echo controller-healthy; }
''', "/etc/init.d/controller-qa", "0100755")
    text("controller-reject", "#!/sbin/openrc-run\nstart() { return 7; }\n", "/etc/init.d/controller-reject", "0100755")
    text("controller-dependent", "#!/sbin/openrc-run\ndepend() { need controller-reject; }\nstart() { touch /run/controller-should-not-exist; }\n", "/etc/init.d/controller-dependent", "0100755")
    text("controller-slow", "#!/sbin/openrc-run\nstart() { echo CONTROLLER-SLOW; sleep 45; touch /run/controller-should-not-exist; }\n", "/etc/init.d/controller-slow", "0100755")
    text("controller-config", "# retained comment\nvalue=old\n", "/etc/conf.d/controller-qa")
    text("controller-log", "OpenRC Controller private log fixture\n", "/var/log/rc.log")
    text("qa-runner", f'''#!/bin/sh
sleep 3
{GUEST}/guest-probe
status=$?
echo "OC_GUEST_DONE status=$status" >/dev/ttyS0
exit "$status"
''', GUEST + "/qa-runner", "0100755")
    text("root-waiter", f'''#!/bin/sh
while [ ! -e /tmp/controller-root-ui ]; do sleep 1; done
export DISPLAY=:0 XAUTHORITY=/home/test/.Xauthority LANG=zh_CN.UTF-8 LC_ALL=zh_CN.UTF-8
echo OC_ROOT_GUI_START >/dev/ttyS0
{APP} >/dev/ttyS0 2>&1
echo "OC_ROOT_GUI_EXIT status=$?" >/dev/ttyS0
''', GUEST + "/root-waiter", "0100755")
    text("acceptance-service", f'''#!/sbin/openrc-run
description="Private controller acceptance"
depend() {{ after localmount; }}
start() {{
  /bin/sh {GUEST}/qa-runner </dev/null >/dev/ttyS0 2>&1 &
  /bin/sh {GUEST}/root-waiter </dev/null >/dev/ttyS0 2>&1 &
  return 0
}}
''', "/etc/init.d/controller-acceptance", "0100755")
    link = "/etc/runlevels/default/controller-acceptance"
    commands.append(f"symlink {link} /etc/init.d/controller-acceptance")
    text("gui-runner", f'''#!/bin/sh
export LANG=zh_CN.UTF-8 LC_ALL=zh_CN.UTF-8
# The private root-account UI fixture needs this session's actual XDM cookie.
if [ -n "${{XAUTHORITY:-}}" ] && [ "$XAUTHORITY" != "$HOME/.Xauthority" ]; then
  cp "$XAUTHORITY" "$HOME/.Xauthority" || exit 1
  chmod 600 "$HOME/.Xauthority" || exit 1
fi
{GUEST}/window-probe >/dev/ttyS0 2>&1 &
watcher=$!
{APP} >/dev/ttyS0 2>&1 &
pid=$!
echo "OC_GUI_START pid=$pid" >/dev/ttyS0
wait "$pid"
echo "OC_GUI_EXIT status=$?" >/dev/ttyS0
kill "$watcher" 2>/dev/null
''', "/usr/bin/ocqa", "0100755")
    script = output / "inject.commands"
    script.write_text("\n".join(dict.fromkeys(commands)) + "\n")
    with (output / "inject.log").open("w") as log:
        run(["debugfs", "-w", "-f", script, root], stdout=log, stderr=subprocess.STDOUT)
    disk.put_root_back(raw, root, layout)
    evidence["layout"] = layout
    (output / "prepare.json").write_text(json.dumps(evidence, ensure_ascii=False, indent=2) + "\n")
    return evidence


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "out/xorg-vmware-debug")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--keep-running", action="store_true", help="Keep this private guest for manual UI checks")
    args = parser.parse_args()
    output = args.output.resolve()
    if not output.is_relative_to(ROOT / "out") or output.exists():
        parser.error("output must be a new directory below out/")
    os.environ["TMPDIR"] = str(ROOT / "out/tmp")
    evidence = prepare(args.build_dir.resolve(), output)
    print("Private image prepared:", output, flush=True)
    if args.prepare_only:
        return
    # The UI walk-through can be resumed by attaching Probe to the recorded QMP socket.
    qmp = ROOT.parent / f".reliefos-openrc-{os.getpid()}.sock"
    command = ["qemu-system-x86_64", "-enable-kvm", "-cpu", "host", "-machine", "q35", "-m", "4096", "-smp", "1",
               "-bios", "/usr/share/edk2/x64/OVMF.4m.fd", "-display", "none", "-serial", f"file:{output / 'serial.log'}",
               "-vga", "vmware", "-device", "qemu-xhci", "-device", "usb-tablet", "-netdev", "user,id=net0", "-device", "e1000,netdev=net0",
               "-drive", f"file={output / 'guest.raw'},format=raw,if=ide", "-qmp", f"unix:{qmp},server=on,wait=off", "-no-reboot", "-no-shutdown"]
    log = (output / "qemu.log").open("w")
    process = subprocess.Popen(command, stdout=log, stderr=log, cwd=ROOT, start_new_session=True)
    (output / "qemu.json").write_text(json.dumps({"pid": process.pid, "qmp": str(qmp), "command": command}, indent=2))
    probe = None
    try:
        deadline = time.monotonic() + 15
        while not qmp.exists():
            if process.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("QEMU failed to start")
            time.sleep(.1)
        probe = Probe(qmp, output)
        serial = output / "serial.log"
        xorg.wait_serial_after(serial, "OC_GUEST_DONE", process, 0, timeout=600)
        text = serial.read_text(errors="replace")
        assert "OC_GUEST_DONE status=0" in text and "OPENRC-CONTROLLER PASS" in text, text[-4000:]
        evidence["checks"].append("real OpenRC worker acceptance")
        print("Real OpenRC worker acceptance passed", flush=True)
        login(probe, process)
        print("Desktop ready; QMP:", qmp, flush=True)
        offset = len(serial.read_text(errors="replace"))
        probe.click(300, 200)
        probe.text("ocqa")
        probe.key("ret")
        xorg.wait_serial_after(serial, "title=OpenRC 权限认证", process, offset, timeout=120)
        native = re.findall(r"OC_WINDOW id=\d+ pid=(\d+) x=(-?\d+) y=(-?\d+) w=540 h=280 title=OpenRC 权限认证", serial.read_text(errors="replace")[offset:])
        assert native, "native authentication window was not observed"
        _, x, y = map(int, native[-1])
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            frame = probe.frame("authentication")
            prompt = frame.crop((x + 20, y + 16, x + 520, y + 62))
            if len(prompt.getcolors(100000) or []) > 4 and min(frame.getpixel((x + 170, y + 138))) > 200:
                break
            time.sleep(.3)
        else:
            raise AssertionError("authentication window did not paint")
        evidence["checks"].append("Chinese unprivileged authentication window painted")
        evidence["guest_checks"] = int(re.search(r"OPENRC-CONTROLLER PASS checks=(\d+)", text)[1])
        evidence["vmware_verified"] = False
        (output / "verification.json").write_text(json.dumps(evidence, ensure_ascii=False, indent=2) + "\n")

    finally:
        if probe is not None:
            probe.stream.close()
            probe.socket.close()
        if not args.keep_running:
            qmp_quit(qmp, process)
        log.close()


if __name__ == "__main__":
    main()
