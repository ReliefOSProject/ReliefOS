#!/usr/bin/env python3
"""Run a real modular/builtin desktop using an immutable owned QEMU snapshot.

This OS integration probe consumes the kernel's actual installed manifest; it
owns no private kernel test implementation. Each invocation is one matrix row.
It exercises normal OpenRC/XDM startup, typed TTY login, current module identity,
local TCP payload integrity, then the non-root IceWM session and a desktop app.
"""
import argparse
from contextlib import contextmanager
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import subprocess
import threading
import time
import uuid

from test_installer_accounts_qemu import Probe
from test_installer_window_qemu import wait_log
from test_vt_qemu import wait_text, ocr
from test_xorg_vm_qemu import collect, greeter_login, wait_greeter_stage, wait_session, session_st_bounds

ROOT = Path(__file__).resolve().parents[1]


def guest_output(raw):
    """Remove complete tagged kernel sideband, preserving all guest output."""
    sgr = re.compile(r"\x1b\[[0-9;]*m")
    kernel_line = re.compile(r"^\[\s*[0-9]+\.[0-9]+\] \[(?:DEBUG|INFO|WARN|ERROR)\] \[reliefnt\] .*$")
    return "".join(line for line in raw.splitlines(keepends=True)
                   if not kernel_line.fullmatch(sgr.sub("", line.rstrip("\r\n"))))


def capture_guest(probe, serial, process, output, label, command, timeout=30):
    """Retain the raw collector result alongside its filtered command output."""
    # Group command lists so every stage reaches the collector, including
    # stderr. Without grouping, the trailing redirect covers only the last
    # command of a list or an AND pipeline.
    body = "(" + command + ") 2>/dev/ttyS0"
    raw = collect(probe, serial, process, label, command=body, timeout=timeout)
    (output / (label.lower() + "-raw.txt")).write_text(raw + "\n")
    actual = guest_output(raw)
    (output / (label.lower() + ".txt")).write_text(actual + "\n")
    return actual


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def require_graphical_identity(output):
    """Reject a restored image or root text shell as graphical input readiness."""
    lines = output.strip().splitlines()
    if len(lines) != 2 or lines[0] != "1000" or not re.fullmatch(r"/dev/pts/[0-9]+", lines[1]):
        raise AssertionError("graphical input did not reach the non-root terminal: " + repr(output))
    return {"uid": 1000, "tty": lines[1]}


def session_terminal_focused(frame):
    """Recognize st's active title in the shipped 100x32+0+0 session fixture.

    A visible saved terminal surface says nothing about the X server's input
    acquisition. Fileman can remain active beside it after a dropped click.
    Require the terminal's own active title, away from its title glyphs; the
    fixture uses IceWM's bundled blue active / gray inactive title colors.
    """
    if frame.width < 640 or frame.height < 480:
        return False
    samples = [frame.getpixel((x, y))[:3]
               for y in (8, 14, 20) for x in range(120, 600, 4)]
    return sum(b > r + 40 and b > g + 40 for r, g, b in samples) > len(samples) * .8


def session_terminal_inactive(frame):
    """Recognize the fixture's gray inactive title, excluding blank frames."""
    if frame.width < 640 or frame.height < 480:
        return False
    samples = [frame.getpixel((x, y))[:3]
               for y in (8, 14, 20) for x in range(120, 600, 4)]
    return sum(64 <= r <= 190 and max(r, g, b) - min(r, g, b) <= 8
               for r, g, b in samples) > len(samples) * .8


def focus_session_terminal(probe, process, timeout=45):
    """Require an inactive-to-active transition instead of accepting a saved title.

    First select the other mapped window with Alt-Tab. Only after the terminal
    is visibly inactive may a click and its resulting active title confirm
    resumption. A static saved blue title, or a blank framebuffer, cannot pass.
    No commands are typed while probing this reversible focus transition.
    """
    deadline = time.monotonic() + timeout
    inactive_seen = False
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("QEMU exited while focusing the graphical terminal")
        if not inactive_seen:
            probe.key("alt-tab")
            inactive_seen = session_terminal_inactive(probe.frame("session-other-focus"))
        else:
            probe.click(300, 200)
            if session_terminal_focused(probe.frame("session-terminal-focus")):
                return
        time.sleep(.5)
    raise AssertionError("the restored session did not confirm a fresh terminal focus transition")


def graphical_marker_visible(text, marker):
    """Accept a fresh shell output line, never the command echo containing it."""
    return any(line.strip() == marker for line in text.splitlines())


def graphical_terminal_text(frame):
    """Read the terminal client alone so adjacent windows cannot merge lines."""
    bounds = session_st_bounds(frame)
    return ocr(frame.crop(bounds)) if bounds else ""


def wait_graphical_marker(probe, process, marker, timeout=30):
    """Wait for actual terminal output after typing in the restored X session."""
    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("QEMU exited during graphical input confirmation")
        last = graphical_terminal_text(probe.frame("graphical-command-output"))
        if graphical_marker_visible(last, marker):
            return
        time.sleep(.5)
    raise AssertionError("no fresh graphical shell output: " + repr(last))


def root_login(probe, process, result):
    """Admit a real TTY session, retaining an expired first login attempt."""
    result["root_login_attempts"] = attempts = []
    for number in (1, 2):
        label = "tty2-attempt-" + str(number)
        try:
            wait_text(probe, process, label + "-login", "login:", timeout=45)
            # The idle getty may have expired while XDM initialized. Clear
            # partial input and require a fresh password and shell response.
            probe.key("ctrl-u")
            probe.text("root"); probe.key("ret")
            wait_text(probe, process, label + "-password", "Password:", timeout=30)
            probe.text("root"); probe.key("ret")
            wait_text(probe, process, label + "-shell", "root@", timeout=45)
            attempts.append({"attempt": number, "passed": True})
            return
        except AssertionError as error:
            attempts.append({"attempt": number, "passed": False, "error": str(error)})
            if number == 2:
                raise


@contextmanager
def local_payload_server():
    """Expose only deterministic test bytes on loopback, never a host directory."""
    payload = bytes(range(256)) * 4096

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path != "/module-probe":
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def log_message(self, *arguments):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        yield server.server_port, hashlib.sha256(payload).hexdigest()
    finally:
        server.shutdown()
        server.server_close()
        worker.join(timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True, help="matching kernel install.bundle.json")
    parser.add_argument("--output", type=Path, required=True, help="new evidence directory")
    parser.add_argument("--firmware", choices=("bios", "uefi"), required=True)
    parser.add_argument("--transport", choices=("ide", "ahci", "nvme"), required=True)
    parser.add_argument("--cpus", type=int, choices=(1, 4), required=True)
    parser.add_argument("--accel", choices=("kvm", "tcg"), default="kvm")
    parser.add_argument("--vga", choices=("std", "vmware"), default="vmware")
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--benchmark", action="store_true", help="measure a bounded 256-MiB yes/dd pipeline")
    args = parser.parse_args()
    image, output = args.image.resolve(), args.output.resolve()
    if not image.is_file() or args.image.is_symlink():
        parser.error("image must be an existing regular private build artifact")
    if not 60 <= args.timeout <= 600:
        parser.error("timeout must be 60..600 seconds")
    install = json.loads(args.manifest.read_bytes())
    release = install["release"]
    if not re.fullmatch(r"[A-Za-z0-9._+-]{1,31}", release):
        parser.error("invalid installed kernel identity")
    output.mkdir(parents=True, exist_ok=False)
    before = digest(image)
    socket_path = Path(os.environ.get("RELIEFOS_QMP_DIR", ROOT.parent)).resolve() / (".reliefos-ko-desktop-" + uuid.uuid4().hex[:12] + ".sock")
    # The socket stays on the project data filesystem, outside deep O= paths.
    if socket_path.exists() or len(os.fsencode(socket_path)) >= 104:
        parser.error("owned QMP socket is unavailable")
    serial = output / "serial.log"
    command = ["qemu-system-x86_64", "-machine", "pc" if args.transport == "ide" else "q35",
               "-accel", "kvm" if args.accel == "kvm" else "tcg,thread=multi",
               "-cpu", "host" if args.accel == "kvm" else "max", "-m", "4096", "-smp", str(args.cpus),
               "-display", "none", "-serial", "file:" + str(serial), "-vga", args.vga,
               "-netdev", "user,id=net0", "-device", "e1000,netdev=net0",
               "-object", "filter-dump,id=netcapture,netdev=net0,file=" + str(output / "network.pcap"),
               "-device", "piix3-usb-uhci", "-device", "usb-tablet",
               "-qmp", "unix:" + str(socket_path) + ",server=on,wait=off", "-no-reboot", "-no-shutdown"]
    if args.firmware == "uefi":
        command += ["-bios", os.environ.get("QEMU_FIRMWARE", "/usr/share/edk2/x64/OVMF.4m.fd")]
    if args.transport == "nvme":
        if args.firmware == "bios":
            # SeaBIOS cannot boot its NVMe driver with the controller BAR
            # above 4 GiB. Keep firmware MMIO in its supported address range;
            # this changes the VM firmware layout, not the guest driver.
            command += ["-fw_cfg", "name=opt/org.seabios/pci64,string=0"]
        command += ["-drive", f"file={image},format=raw,if=none,id=osdisk,snapshot=on",
                    "-device", "nvme,drive=osdisk,serial=reliefos-module-test,bootindex=1"]
    else:
        command += ["-drive", f"file={image},format=raw,if=ide,snapshot=on"]
    result = {"passed": False, "image": str(image), "image_sha256": before,
              "release": release, "module_abi": install["module_abi"], "firmware": args.firmware,
              "transport": args.transport, "cpus": args.cpus, "accelerator": args.accel, "vga": args.vga,
              "command": command, "ownership": "only this process; immutable source snapshot"}
    (output / "owned.json").write_text(json.dumps(result, indent=2) + "\n")
    process = probe = None
    root_tty = False
    started = time.monotonic()
    try:
        with (output / "qemu.log").open("w") as log:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 15
        while not socket_path.exists():
            if process.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("owned QEMU did not create QMP; see qemu.log")
            time.sleep(.1)
        probe = Probe(socket_path, output)
        wait_log(serial, "boot complete:", process, timeout=args.timeout)
        if "version=" + release not in serial.read_text(errors="replace"):
            raise AssertionError("actual boot release differs from supplied install manifest")
        print("BOOT", release, args.firmware, args.transport, args.cpus, flush=True)
        wait_greeter_stage(probe, process, "login", "xdm-greeter", timeout=args.timeout)
        result["greeter_seconds"] = round(time.monotonic() - started, 3)
        # No debug-only VT trace is required in a release kernel. A fresh shell
        # marker and the visible greeter prove the actual VT handoffs instead.
        probe.key("ctrl-alt-f2")
        root_login(probe, process, result)
        root_tty = True
        print("TTY2 root login", flush=True)
        configuration = install["configuration"]
        enabled = configuration.get("MODULES") == "y"
        module_query = "cat /proc/modules" if enabled else "test ! -e /proc/modules && echo MODULES-DISABLED"
        identity = capture_guest(probe, serial, process, output, "KERNEL-IDENTITY",
                                 "uname -r; id -u; " + module_query)
        lines = identity.strip().splitlines()
        if len(lines) < 2 or lines[:2] != [release, "0"]:
            raise AssertionError("typed normal TTY session has a wrong kernel or root identity")
        modules = "\n".join(lines[2:])
        (output / "modules.txt").write_text(modules + "\n")
        if enabled:
            names = {line.split()[0] for line in modules.splitlines() if line.split()}
            root_option = {"ide": "PATA_IDE", "ahci": "SATA_AHCI", "nvme": "BLK_DEV_NVME"}[args.transport]
            required = {name for option, name in (("EXT_FS_COMMON", "ext_common"),
                        ("EXT4_FS", "ext4"), (root_option, args.transport), ("E1000", "e1000"))
                        if configuration.get(option) == "m"}
            if not required <= names:
                raise AssertionError("normal root modules not loaded: " + str(sorted(required - names)))
        elif modules.strip() != "MODULES-DISABLED":
            raise AssertionError("MODULES=n unexpectedly exposes a module list")
        print("Kernel/module identity", flush=True)
        root_tty = False
        probe.key("ctrl-alt-f1")
        wait_greeter_stage(probe, process, "login", "xdm-after-tty", timeout=args.timeout)
        session_started = time.monotonic()
        greeter_login(probe, process, timeout=args.timeout)
        wait_session(probe, process, "icewm-session", timeout=args.timeout)
        result["session_seconds"] = round(time.monotonic() - session_started, 3)
        print("IceWM non-root session", flush=True)
        probe.click(300, 200)
        probe.text("id -u >/home/test/moduleuid; echo reliefos-module-files-ok >/home/test/modulecheck; fileman >/tmp/module-fileman.log 2>&1 &")
        probe.key("ret")
        probe.frame("icewm-after-app-launch")
        probe.key("ctrl-alt-f2")
        wait_text(probe, process, "tty2-return", "root@", timeout=30)
        root_tty = True
        processes = capture_guest(probe, serial, process, output, "DESKTOP-PROCESSES", "ps")
        (output / "processes.txt").write_text(processes + "\n")
        for name in ("Xorg", "icewm", "fileman"):
            if name not in processes:
                raise AssertionError("desktop process missing: " + name)
        fileman = capture_guest(probe, serial, process, output, "FILEMAN-LOG",
                               "cat /tmp/module-fileman.log; echo DESKTOP-USER-ID; cat /home/test/moduleuid; echo DESKTOP-FILE; cat /home/test/modulecheck")
        if (not re.search(r"list path=/home/test count=[1-9][0-9]*", fileman) or
                "FLTK file manager ready" not in fileman):
            raise AssertionError("Fileman did not enumerate the real non-root home")
        if not fileman.strip().endswith("DESKTOP-USER-ID\n1000\nDESKTOP-FILE\nreliefos-module-files-ok"):
            raise AssertionError("desktop UID or non-root write/read mismatch")
        # The app's flushed ready marker follows show(). Capture the visible
        # desktop after that proof, rather than matching its name in the shell
        # command that launched it.
        root_tty = False
        switched = time.monotonic()
        probe.key("ctrl-alt-f1")
        wait_session(probe, process, "icewm-fileman", timeout=45)
        # A saved framebuffer is restored before Xorg handles its acquire
        # signal. A partially copied terminal can already pass the mapped-
        # window predicate. Require fresh input in the actual user's PTY
        # before asking Xorg to release the display again; no fixed sleep or
        # kernel DEBUG trace is used as an acquisition acknowledgement.
        focus_session_terminal(probe, process)
        nonce = uuid.uuid4().hex[:8]
        marker = "KOREADY"
        receipt = "/home/test/kr" + nonce
        # The visible acknowledgement is absent before this command. Its
        # unique file independently proves UID/PTY after the return switch;
        # do not require OCR to distinguish tiny hexadecimal font glyphs.
        if graphical_marker_visible(graphical_terminal_text(probe.frame("before-graphical-command")), marker):
            raise AssertionError("graphical acknowledgement already exists before fresh input")
        # The normal user's home is the return channel. Do not assume that a
        # desktop account can open the machine's serial device for writing.
        # Keep this shorter than the known 100-column st plus its prompt so
        # command echo cannot wrap into the exact output line we require.
        probe.text("(id -u; tty) >" + receipt + "; echo " + marker)
        probe.key("ret")
        wait_graphical_marker(probe, process, marker)
        returned = time.monotonic()
        probe.key("ctrl-alt-f2")
        wait_text(probe, process, "tty2-after-fileman", "root@", timeout=30)
        root_tty = True
        returned_seconds = time.monotonic() - returned
        roundtrip_seconds = time.monotonic() - switched
        resumed = capture_guest(probe, serial, process, output, "GRAPHICAL-RESUMED",
                                "cat " + receipt, timeout=30)
        result["graphical_resumption"] = require_graphical_identity(resumed)
        result["desktop_vt_return"] = {
            "seconds": round(returned_seconds, 3),
            "measurement": "host wall time from the release key to visible text after fresh graphical input proof"}
        result["desktop_vt_roundtrip"] = {
            "seconds": round(roundtrip_seconds, 3),
            "measurement": "host wall time for two VT switches plus a fresh typed UID/PTY command, QMP and image/OCR sampling"}
        capture_guest(probe, serial, process, output, "NETWORK-STATE",
                      "busybox ifconfig; busybox route -n")
        with local_payload_server() as (port, expected):
            # The full profile ships GNU wget; BusyBox's WGET applet is disabled.
            # Use volatile /run so this network oracle does not also time a
            # slow PIO root disk. Real-root writes were checked above. Keep
            # the complete payload, its digest and both request/process bounds.
            body = f"timeout 20 wget --no-proxy -T 10 -t 1 -O /run/module-network.bin http://10.0.2.2:{port}/module-probe && sha256sum /run/module-network.bin"
            network = capture_guest(probe, serial, process, output, "LOCAL-TCP", body, timeout=60)
            (output / "network.txt").write_text(network + "\n")
            if not re.search(r"(?m)^" + expected + r"  /run/module-network\.bin$", network):
                raise AssertionError("normal OpenRC network did not transfer the complete local payload")
        print("Local TCP SHA256", flush=True)
        if args.benchmark:
            measurement = capture_guest(probe, serial, process, output, "YES-DD",
                "LC_ALL=C timeout 30 sh -c 'yes | dd of=/dev/null bs=64K count=4096 iflag=fullblock status=progress'; echo BENCHMARK-STATUS=$?", timeout=60)
            if not re.search(r"(?m)^BENCHMARK-STATUS=0$", measurement):
                raise AssertionError("bounded yes/dd pipeline did not finish successfully")
            match = re.search(r"268435456 bytes[^\r\n]*copied,\s*([0-9.]+)\s*s,", measurement)
            if not match or float(match[1]) <= 0:
                raise AssertionError("dd did not report the complete measured byte count and duration")
            seconds = float(match[1])
            result["yes_dd"] = {"bytes": 268435456, "guest_seconds": seconds,
                                "bytes_per_second": 268435456 / seconds,
                                "block_bytes": 65536, "fullblock": True}
            print("YES-DD", result["yes_dd"], flush=True)
        registers = []
        for cpu in range(args.cpus):
            state = probe.command("human-monitor-command", {"command-line": "info registers", "cpu-index": cpu})
            cr0 = int(re.search(r"CR0=([0-9a-fA-F]+)", state)[1], 16)
            efer = int(re.search(r"EFER=([0-9a-fA-F]+)", state)[1], 16)
            registers.append({"cpu": cpu, "wp": bool(cr0 & (1 << 16)), "nxe": bool(efer & (1 << 11))})
        result["cpu_protection"] = registers
        if not all(state["wp"] and state["nxe"] for state in registers):
            raise AssertionError("kernel/module protection lost on a virtual CPU")
        result["passed"] = True
    except Exception as error:
        result["error"] = str(error)
        if probe and process and process.poll() is None:
            try:
                # Preserve the failing graphical state before diagnostics
                # deliberately switch away to the existing root text VT.
                probe.frame("failure-before-diagnostics")
                if not root_tty:
                    probe.key("ctrl-alt-f2")
                    if any(attempt.get("passed") for attempt in result.get("root_login_attempts", [])):
                        wait_text(probe, process, "failure-existing-root", "root@", timeout=30)
                    else:
                        diagnosis = {}
                        root_login(probe, process, diagnosis)
                        result["failure_root_login_attempts"] = diagnosis["root_login_attempts"]
                    root_tty = True
                capture_guest(probe, serial, process, output, "FAILURE-STARTUP",
                              "echo XORG-LOG; cat /var/log/Xorg.0.log; "
                              "echo XDM-LOG; cat /var/log/xdm.log /var/log/xdm-error.log; "
                              "echo DEVICES; ls -l /dev/fb0 /dev/dri /dev/ttyS0; "
                              "echo XSESSION; cat /home/test/.xsession-errors; echo PROCESSES; ps", timeout=60)
                text = serial.read_text(errors="replace")
                pids = {}
                for pid, name in re.findall(r"exec pid=([0-9]+) path=(/usr/bin/icewm|/usr/bin/st|/usr/lib/reliefos/apps/fileman/fileman\.elf)\b", text):
                    pids[name] = pid
                command = "; ".join("echo PID-" + pid + "; cat /proc/" + pid + "/stat /proc/" + pid + "/status" for pid in pids.values())
                if command:
                    capture_guest(probe, serial, process, output, "FAILURE-PROCESSES", command, timeout=45)
            except Exception as diagnostic_error:
                result["diagnostic_error"] = str(diagnostic_error)
    finally:
        if probe:
            try:
                probe.frame("last-frame")
            except Exception as error:
                result["screenshot_error"] = str(error)
            probe.close()
        if process and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait()
        socket_path.unlink(missing_ok=True)
        result["source_unchanged"] = digest(image) == before
        result["passed"] = result["passed"] and result["source_unchanged"]
        (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, ensure_ascii=False), flush=True)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
