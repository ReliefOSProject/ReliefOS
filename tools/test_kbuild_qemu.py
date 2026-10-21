#!/usr/bin/env python3
"""Boot a freshly built image; verify shipped bytes, POSIX I/O probes and GUI."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

from test_installer_accounts_qemu import Probe
from run_gcc_probe_qemu import qmp_quit
import test_vdso_qemu as disk_tools
import test_xorg_vm_qemu as xorg

ROOT = Path(__file__).resolve().parents[1]
GUEST = "/usr/lib/reliefos/tests"
SERVICE = "/etc/init.d/reliefos-kbuild-qa"
LINK = "/etc/runlevels/default/reliefos-kbuild-qa"


def run(command, **kwargs):
    return subprocess.run([str(item) for item in command], check=True, cwd=ROOT, **kwargs)


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def close_button(frame) -> tuple[int, int]:
    """Locate the active IceWM blue title strip and its native close button."""
    bands = []
    for y in range(frame.height - 30):
        xs = [x for x in range(frame.width)
              if (lambda c: c[2] >= 96 and c[2] > c[0] + 60 and c[2] > c[1] + 30)
              (frame.getpixel((x, y)))]
        if len(xs) < frame.width * .35:
            continue
        if not bands or y > bands[-1][-1][0] + 2:
            bands.append([])
        bands[-1].append((y, max(xs)))
    for band in bands:
        top, bottom = band[0][0], band[-1][0]
        height = bottom - top + 1
        if height < 10 or height > 32:
            continue
        right = max(item[1] for item in band)
        return min(frame.width - 10, round(right + height * 2.5)), (top + bottom) // 2
    raise AssertionError("active IceWM title strip was not visible")


def read_guest(root: Path, path: str, destination: Path) -> None:
    result = run(["debugfs", "-R", f'dump -p {path} "{destination}"', root],
                 text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if not destination.is_file():
        raise RuntimeError(f"cannot read {path}: {result.stdout}")


def prepare(build: Path, output: Path, source: Path) -> tuple[Path, dict]:
    raw, layout = disk_tools.private_raw_image(source, output)
    root = disk_tools.extract_root(raw, layout, output)
    records = {}
    # Validate the image's kernel and programs before adding any test files.
    # Replacing them here would hide a stale image dependency.
    kernel = output / "shipped-kernel.sys"
    run(["mcopy", "-i", f'{raw}@@{int(layout["esp_first"]) * 512}',
         "::/reliefos/kernel.sys", kernel])
    expected_kernel = build / "generated/system/kernel.sys"
    if digest(kernel) != digest(expected_kernel):
        raise AssertionError("shipped kernel differs from current Kbuild product")
    records["kernel.sys"] = digest(kernel)
    for name in ("fileman", "taskmgr"):
        readback = output / f"shipped-{name}.elf"
        read_guest(root, f"/usr/lib/reliefos/apps/{name}/{name}.elf", readback)
        if digest(readback) != digest(build / f"userland/{name}.elf"):
            raise AssertionError(f"shipped {name} differs from current Kbuild product")
        records[name] = digest(readback)
    readback = output / "shipped-runtime.so"
    read_guest(root, "/usr/lib/reliefos/libreliefos.so.2", readback)
    if digest(readback) != digest(build / "system/lib/libreliefos.so.2"):
        raise AssertionError("shipped runtime differs from current Kbuild product")
    records["runtime"] = digest(readback)

    compiler = build / "sdk/reliefos-musl-sdk/bin/reliefos-musl-cc"
    commands = []
    disk_tools.add_missing_root_directory(root, GUEST, commands)
    disk_tools.add_missing_root_directory(root, "/home/test", commands)
    commands.extend(["set_inode_field /home/test uid 1000", "set_inode_field /home/test gid 1000",
                     "set_inode_field /home/test mode 040700"])

    def put(file: Path, guest: str, mode: str):
        if disk_tools.debugfs_exists(root, guest):
            commands.append(f"rm {guest}")
        commands.extend([f'write "{file}" {guest}', f"set_inode_field {guest} mode {mode}"])

    for kind in ("static", "dynamic"):
        binary = output / f"io-runtime-{kind}.elf"
        run([compiler, "-std=gnu11", "-O2",
             *(["-static"] if kind == "static" else []), ROOT / "tools/tests/syscall_io_runtime_probe.c",
             "-o", binary], stdout=(output / f"compile-{kind}.log").open("w"), stderr=subprocess.STDOUT)
        put(binary, f"{GUEST}/{binary.name}", "0100755")
        records[f"probe-{kind}"] = digest(binary)
    # One foreground runner owns both launches. Keyboard input is only needed
    # at the initial terminal; later launches follow confirmed process exits.
    desktop_runner = output / "desktop-runner"
    desktop_runner.write_text('''#!/bin/sh
export LANG=zh_CN.UTF-8 LC_ALL=zh_CN.UTF-8
echo "KBUILD-GUI-START" >/dev/ttyS0
for app in fileman taskmgr; do
    /usr/bin/$app >/dev/ttyS0 2>&1 &
    child=$!
    echo "[kbuild-gui] PID app=$app pid=$child" >/dev/ttyS0
    wait "$child"
    status=$?
    echo "[kbuild-gui] EXIT app=$app pid=$child status=$status" >/dev/ttyS0
done
''')
    put(desktop_runner, f"{GUEST}/desktop-runner", "0100755")
    # Add only the public test fixture user, preserving all shipped package users.
    for database in ("passwd", "shadow", "group", "gshadow"):
        file = output / database
        read_guest(root, f"/etc/{database}", file)
        lines = [line for line in file.read_text().splitlines() if not line.startswith("test:")]
        fixture = (ROOT / "system/test-accounts" / database).read_text().splitlines()
        lines.extend(line for line in fixture if line.startswith("test:"))
        file.write_text("\n".join(lines) + "\n")
        put(file, f"/etc/{database}", "0100600" if "shadow" in database else "0100644")

    service = output / "reliefos-kbuild-qa"
    service.write_text(f'''#!/sbin/openrc-run
description="Kbuild private guest acceptance"
depend() {{ after localmount; before reliefos-session; }}
start() {{
    echo "[kbuild-qa] BEGIN static" >/dev/ttyS0
    {GUEST}/io-runtime-static.elf >/dev/ttyS0 2>&1
    static_status=$?
    echo "[kbuild-qa] END static status=$static_status" >/dev/ttyS0
    echo "[kbuild-qa] BEGIN dynamic" >/dev/ttyS0
    {GUEST}/io-runtime-dynamic.elf >/dev/ttyS0 2>&1
    dynamic_status=$?
    echo "[kbuild-qa] END dynamic status=$dynamic_status" >/dev/ttyS0
    echo "[kbuild-qa] DONE static=$static_status dynamic=$dynamic_status" >/dev/ttyS0
    return 0
}}
''')
    put(service, SERVICE, "0100755")
    if disk_tools.debugfs_exists(root, LINK):
        commands.append(f"rm {LINK}")
    commands.append(f"symlink {LINK} {SERVICE}")
    script = output / "inject.commands"
    script.write_text("\n".join(commands) + "\n")
    with (output / "inject.log").open("w") as log:
        run(["debugfs", "-w", "-f", script, root], stdout=log, stderr=subprocess.STDOUT)
    disk_tools.put_root_back(raw, root, layout)
    return raw, records


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=480)
    args = parser.parse_args()
    build, output = args.build_dir.resolve(), args.output.resolve()
    if not output.is_relative_to(ROOT / "out") or output.exists():
        parser.error("--output must be a new directory below out/")
    if len(str(output / "qmp.sock").encode()) >= 108:
        parser.error("use a shorter output path for the QMP socket")
    output.mkdir(parents=True)
    temporary = ROOT / "out/tmp"
    temporary.mkdir(parents=True, exist_ok=True)
    os.environ["TMPDIR"] = str(temporary)
    source = build / "images/reliefos.vmdk"
    metadata = {"source_image": str(source), "source_sha256": digest(source), "checks": []}
    process = probe = None
    try:
        raw, metadata["shipped_sha256"] = prepare(build, output, source)
        process, serial, qmp = xorg.boot([], raw, output)
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            text = serial.read_text(errors="replace") if serial.exists() else ""
            if "[kbuild-qa] DONE" in text:
                break
            if process.poll() is not None or "KERNEL PANIC" in text:
                raise RuntimeError("guest exited or panicked")
            time.sleep(.5)
        else:
            raise TimeoutError("POSIX I/O guest probes did not finish")
        for kind in ("static", "dynamic"):
            block = re.search(r"\[kbuild-qa\] BEGIN " + kind + r"\r?\n(.*?)"
                              r"\[kbuild-qa\] END " + kind + r" status=(\d+)", text, re.S)
            if block is None or block[2] != "0":
                raise AssertionError(f"{kind}: POSIX I/O runner did not succeed")
            counts = re.findall(r"\[io-runtime\] DONE checks=(\d+) failures=0", block[1])
            if counts != ["52"] or "[io-runtime] FAIL" in block[1]:
                raise AssertionError(f"{kind}: expected 52 successful I/O checks, observed {block[1]}")
            metadata["checks"].append({"POSIX I/O": kind, "passed": 52})
        if "[kbuild-qa] DONE static=0 dynamic=0" not in text:
            raise AssertionError("guest POSIX I/O runner returned failure")
        probe = Probe(qmp, output)
        probe.frame("xdm")
        xorg.greeter_login(probe, process)
        xorg.wait_session(probe, process, "icewm", timeout=90)
        probe.click(300, 200)
        probe.key("ctrl-u")
        xorg.st_marker(probe, serial, process, "KBUILD-GUI-START",
                       f"sh {GUEST}/desktop-runner")
        for app in ("fileman", "taskmgr"):
            ready = "FLTK file manager ready" if app == "fileman" else "FLTK task manager ready"
            end = time.monotonic() + 30
            while time.monotonic() < end:
                if ready in serial.read_text(errors="replace"):
                    break
                if process.poll() is not None:
                    raise RuntimeError("guest exited while launching GUI")
                time.sleep(.2)
            else:
                raise AssertionError(f"{app} did not reach its real GUI ready marker")
            expected = "新建文件夹" if app == "fileman" else "刷新"
            end = time.monotonic() + 45
            while time.monotonic() < end:
                frame = probe.frame(f"desktop-{app}")
                ocr_input = output / f"desktop-{app}-ocr.png"
                frame.convert("L").resize((frame.width * 3, frame.height * 3)).save(ocr_input)
                result = run(["tesseract", ocr_input, "stdout", "-l", "chi_sim+eng", "--psm", "11"],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=15)
                (output / f"desktop-{app}.ocr.txt").write_text(result.stdout)
                if expected in re.sub(r"\s+", "", result.stdout):
                    break
                if process.poll() is not None:
                    raise RuntimeError("guest exited before its GUI was painted")
                time.sleep(.4)
            else:
                raise AssertionError(f"{app} never painted its expected control {expected!r}")
            metadata["checks"].append({"GUI": app, "ready": True, "visible_control": expected})
            # Use the visible window-manager control rather than leave an Alt
            # chord pending while a client is processing its first expose.
            probe.click(*close_button(frame))
            close_marker = f"[kbuild-gui] EXIT app={app} "
            end = time.monotonic() + 20
            while time.monotonic() < end:
                current = serial.read_text(errors="replace")
                closed = re.search(re.escape(close_marker) + r"pid=(\d+) status=(\d+)", current)
                if closed:
                    if closed[2] != "0":
                        raise AssertionError(f"{app} exited with status {closed[2]}")
                    metadata["checks"][-1].update({"pid": int(closed[1]), "exit_status": 0})
                    break
                if process.poll() is not None:
                    raise RuntimeError("guest exited before its application closed")
                time.sleep(.2)
            else:
                raise AssertionError(f"{app} window close was not confirmed")
        metadata["completed"] = True
    except Exception as error:
        metadata["error"] = f"{type(error).__name__}: {error}"
        raise
    finally:
        if probe is not None:
            probe.close()
        if process is not None and process.poll() is None:
            try:
                qmp_quit(output / "qmp.sock", process)
            except Exception:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
        metadata["source_unchanged"] = digest(source) == metadata["source_sha256"]
        (output / "results.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n")
    if not metadata["source_unchanged"]:
        raise AssertionError("source image changed")
    print("PASS: shipped hashes, 52 POSIX I/O checks per linkage mode, XDM/IceWM/fileman/taskmgr")


if __name__ == "__main__":
    main()
