#!/usr/bin/env python3
"""Isolated network fixtures and production binaries on an e1000 QEMU guest.

Run in a private network/user namespace so DNS/NTP fixture ports do not alter
host services. No Internet or host resolver configuration changes are needed.
"""
import argparse
import fcntl
import hashlib
import http.server
import json
import os
from pathlib import Path
import re
import shutil
import socket
import ssl
import struct
import subprocess
import tempfile
import threading
import time
from PIL import Image
import test_linux_ioctl_cloexec as iso_tools
from test_power_qemu import PowerProbe
from test_sudo_e2e_qemu import Probe as GuiProbe
from test_installer_window_qemu import wait_log, title_point

ROOT = Path(__file__).resolve().parents[1]
PAYLOAD = bytes(i % 251 for i in range(1024 * 1024))


def inject(image, source, target, mode="0100644"):
    for command in (f"rm {target}", f"write {source} {target}", f"set_inode_field {target} mode {mode}"):
        subprocess.run(["debugfs", "-w", "-R", command, str(image)], check=True, capture_output=True)
    content = subprocess.check_output(["debugfs", "-R", f"cat {target}", str(image)], stderr=subprocess.DEVNULL)
    assert content == source.read_bytes(), f"image injection failed: {target}"


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    def do_GET(self):
        if self.path == "/downgrade":
            self.send_response(302)
            self.send_header("Location", "http://fixture.test:18080/payload")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Length", str(len(PAYLOAD)))
        self.end_headers()
        try:
            self.wfile.write(PAYLOAD)
        except (BrokenPipeError, ConnectionResetError):
            pass
    def log_message(self, *args):
        pass


def udp_fixture(port, stop, counters):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server:
        server.bind(("0.0.0.0", port))
        server.settimeout(.2)
        while not stop.is_set():
            try:
                packet, peer = server.recvfrom(4096)
            except socket.timeout:
                continue
            counters[str(port)] += 1
            if port == 53:
                if len(packet) < 12: continue
                end = 12
                while end < len(packet) and packet[end]: end += packet[end] + 1
                end += 5
                if end > len(packet): continue
                response = packet[:2] + struct.pack("!5H", 0x8180, 1, 1, 0, 0) + packet[12:end]
                response += b"\xc0\x0c" + struct.pack("!HHIH", 1, 1, 60, 4) + socket.inet_aton("10.37.0.2")
            else:
                if len(packet) < 48: continue
                response = bytearray(48)
                response[0:2] = bytes((0x24, 2))
                response[24:32] = packet[40:48]
                timestamp = struct.pack("!II", (2208988800 + 2208988800) & 0xffffffff, 0x40000000)
                response[32:40] = response[40:48] = timestamp
            server.sendto(response, peer)


def read_taskbar_clock(frame, destination):
    crop = frame.crop((frame.width - 83, frame.height - 25,
                       frame.width - 12, frame.height - 9))
    crop = crop.convert("L").resize((crop.width * 6, crop.height * 6), Image.Resampling.LANCZOS)
    crop.save(destination)
    value = subprocess.check_output(["tesseract", str(destination), "stdout", "--psm", "7",
                                     "-c", "tessedit_char_whitelist=0123456789:"],
                                    text=True, stderr=subprocess.DEVNULL).strip()
    assert re.fullmatch(r"\d{2}:\d{2}:\d{2}", value), f"unreadable taskbar clock: {value!r}"
    hour, minute, second = map(int, value.split(":"))
    assert hour < 24 and minute < 60 and second < 60, value
    return hour * 3600 + minute * 60 + second


def gui_dhcp(qmp, work, serial, process):
    wait_log(serial, "[login.elf] starting login UI", process, timeout=60)
    probe = GuiProbe(qmp, work)
    try:
        time.sleep(2)
        probe.key("down")
        probe.text("test")
        probe.key("ret")
        wait_log(serial, "[pam-login] session ready", process, timeout=30)
        time.sleep(2)
        probe.key("meta_l")
        time.sleep(1)
        probe.text("netctl")
        probe.key("ret")
        wait_log(serial, "[netctl.elf] network controller starting", process, timeout=30)
        time.sleep(2)
        frame = probe.frame("netctl-before")
        x, y = title_point(frame)
        button = (x - 120 + 183, y - 8 + 344)
        for attempt in range(2):
            offset = len(serial.read_text(errors="replace"))
            probe.click(*button)
            deadline = time.monotonic() + 20
            while "path=/usr/lib/reliefos/apps/sudod/sudod.elf" not in serial.read_text(errors="replace")[offset:]:
                if process.poll() is not None or time.monotonic() >= deadline:
                    probe.frame("authorization-missing")
                    raise AssertionError("DHCP button did not request sudo authorization")
                time.sleep(.2)
            time.sleep(1)
            probe.frame(f"authorization-{attempt}")
            if attempt == 0:
                probe.key("esc")
                deadline = time.monotonic() + 20
                while "name=sudo code=1" not in serial.read_text(errors="replace")[offset:]:
                    if process.poll() is not None or time.monotonic() >= deadline:
                        probe.frame("cancel-failed")
                        raise AssertionError("cancelling askpass did not end sudo authorization")
                    time.sleep(.2)
                cancelled = serial.read_text(errors="replace")[offset:]
                assert "path=/sbin/unix_chkpwd" not in cancelled, "cancel submitted a password to PAM"
                assert "name=netctl.elf code=0" not in cancelled, "cancel ran the privileged helper"
                time.sleep(1)
                probe.frame("authorization-cancelled")
            else:
                probe.text("test")
                probe.key("ret")
                deadline = time.monotonic() + 25
                while "name=netctl.elf code=0" not in serial.read_text(errors="replace")[offset:]:
                    if process.poll() is not None or time.monotonic() >= deadline:
                        probe.frame("renew-failed")
                        raise AssertionError("authorized DHCP helper did not succeed")
                    time.sleep(.2)
                time.sleep(2)
                probe.frame("dhcp-renewed")
        # Resolve the controller's default host through the isolated DNS
        # fixture after renewing, without changing its DNS policy.
        probe.click(button[0] + 392, button[1])
        time.sleep(3)
        first = read_taskbar_clock(probe.frame("dns-after-renew"), work / "clock-first.png")
        time.sleep(2)
        second = read_taskbar_clock(probe.frame("clock-next"), work / "clock-second.png")
        assert 0 < first < second < 120 and second - first <= 10, (first, second)
        print(f"[network-gui] PASS taskbar clock advances: {first} -> {second} seconds after fixture midnight")
    finally:
        probe.close()


def main():
    namespace = os.open("/proc/self/ns/net", os.O_RDONLY)
    try:
        owner = fcntl.ioctl(namespace, 0xb701)  # NS_GET_USERNS
        try:
            if os.fstat(owner).st_ino != os.stat("/proc/self/ns/user").st_ino or os.geteuid() != 0:
                raise PermissionError("network namespace is not owned by this mapped user")
        finally: os.close(owner)
    except PermissionError:
        raise SystemExit("Run under: unshare --user --map-root-user --net python3 tools/test_network_qemu.py")
    finally: os.close(namespace)
    subprocess.run(["ip", "link", "set", "lo", "up"], check=True)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nic", choices=("e1000", "e1000-82545em"), default="e1000-82545em")
    parser.add_argument("--smp", type=int, default=2)
    parser.add_argument("--reuse", action="store_true", help="reuse the staged root and ISO")
    parser.add_argument("--gui", action="store_true", help="exercise DHCP, PAM and the taskbar clock (requires tesseract)")
    args = parser.parse_args()
    work = ROOT / ("build/network-gui-test" if args.gui else "build/network-test")
    work.mkdir(exist_ok=True, parents=True)
    certificate, key = work / "fixture.crt", work / "fixture.key"
    iso = work / "network-test.iso"
    if not args.reuse:
        compiler = ROOT / "build/musl-gcc/root/opt/dyne/gcc-musl/bin/x86_64-linux-musl-gcc"
        probe = work / "network.elf"
        subprocess.run([str(compiler), "-static", "-O2", "-Wall", "-D_GNU_SOURCE", "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi",
                        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                        "-idirafter", "userland/runtime/include", "tools/tests/network_guest_test.c",
                        "userland/runtime/src/ntp.c", "userland/runtime/src/netsock.c", "userland/runtime/src/unix_ipc.c",
                        "-Lbuild/musl/lib", "-Wl,--start-group", "-l:libleonos.a",
                        "-Wl,--end-group", "-o", str(probe)], check=True)
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "9000",
                        "-subj", "/CN=fixture.test", "-addext", "subjectAltName=DNS:fixture.test",
                        "-out", str(certificate), "-keyout", str(key)], check=True, capture_output=True)
        ca_bundle = work / "ca-certificates.crt"
        ca_bundle.write_bytes((ROOT / "system/certs/cacert.pem").read_bytes() +
                              certificate.read_bytes())
        image = work / "root.ext2"
        shutil.copy2(ROOT / "build/live/root.ext2", image)
        subprocess.run(["debugfs", "-w", "-R", "mkdir /usr/lib/reliefos/tests", str(image)], check=True, capture_output=True)
        for source, destination in ((probe, "/usr/lib/reliefos/tests/linux-inventory.elf"),
                (ROOT / "build/userland/busybox.elf", "/bin/busybox"),
                (ROOT / "build/system/lib/libleonos.so.2", "/usr/lib/leonos/libleonos.so.2"),
                (ROOT / "build/userland/netctl.elf", "/usr/lib/reliefos/apps/netctl/netctl.elf"),
                (ROOT / "build/userland/sudod.elf", "/usr/lib/reliefos/apps/sudod/sudod.elf"),
                (ROOT / "build/userland/rcctl.elf", "/usr/lib/reliefos/apps/rcctl/rcctl.elf")):
            inject(image, source, destination, "0100755")
        inject(image, ca_bundle, "/etc/ssl/certs/ca-certificates.crt")
        config = work / "ntp.conf"
        config.write_text("server 10.37.0.2\n")
        inject(image, config, "/etc/ntp.conf")
        inject(image, ROOT / "system/config/network.conf", "/etc/reliefos/network.conf")
        if not args.gui:
            policy = work / "network-sudoers"
            policy.write_text("test ALL=(root) NOPASSWD: /usr/lib/reliefos/apps/netctl/netctl.elf --renew-dhcp\n")
            inject(image, policy, "/etc/sudoers.d/network-test", "0100440")
        inject(image, certificate, "/tmp/net-cert.pem")
        script = work / "https.py"
        script.write_text("import ssl, urllib.request\n"
            "context = ssl.create_default_context(cafile='/tmp/net-cert.pem')\n"
            "with urllib.request.urlopen('https://fixture.test:18443/payload', context=context, timeout=20) as response:\n"
            "    data = response.read()\n"
            "assert data == bytes(i % 251 for i in range(1024*1024))\n"
            "try:\n"
            "    urllib.request.urlopen('https://fixture.test:18443/payload', timeout=20)\n"
            "except urllib.error.URLError as error:\n"
            "    assert isinstance(error.reason, ssl.SSLCertVerificationError), error\n"
            "else:\n"
            "    raise AssertionError('untrusted certificate accepted')\n"
            "print('[network] PYTHON_HTTPS_OK verified 1048576 bytes', flush=True)\n")
        inject(image, script, "/tmp/net-https.py")
        iso_tools.GRUB_TEMPLATE = iso_tools.GRUB_TEMPLATE.replace("set timeout=5", "set timeout=0").replace(
            "autospawn=ioctlcloexec autospawn=python315", "" if args.gui else "autospawn=inventory").replace("syscall-trace=/opt/python/", "")
        iso_tools.build_iso(image, iso, work / "grub.cfg", work)
    stop = threading.Event()
    counters = {"53": 0, "123": 0}
    threads = [threading.Thread(target=udp_fixture, args=(port, stop, counters)) for port in (53, 123)]
    servers = [http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler) for port in (18080, 18443)]
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(certificate, key)
    servers[1].socket = context.wrap_socket(servers[1].socket, server_side=True)
    threads += [threading.Thread(target=s.serve_forever) for s in servers]
    for thread in threads: thread.start()
    serial = work / f"{args.nic}-smp{args.smp}.log"
    serial.write_text("")
    try:
        with tempfile.TemporaryDirectory(prefix="net-qmp-") as temporary, (work / "qemu.log").open("w") as errors:
            qmp = Path(temporary) / "qmp"
            command = ["qemu-system-x86_64", "-enable-kvm", "-cpu", "host", "-machine", "q35", "-m", "4096",
                "-smp", str(args.smp), "-bios", "/usr/share/edk2/x64/OVMF.4m.fd", "-display", "none",
                "-serial", f"file:{serial}", "-device", "VGA,xres=1280,yres=720", "-cdrom", str(iso), "-boot", "d",
                "-netdev", "user,id=net0,net=10.37.0.0/24", "-device", f"{args.nic},netdev=net0,id=testnic",
                "-qmp", f"unix:{qmp},server=on,wait=off", "-no-reboot", "-no-shutdown"]
            process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=errors)
            try:
                if args.gui:
                    gui_dhcp(qmp, work, serial, process)
                deadline = time.monotonic() + 140
                changed = set()
                while not args.gui and process.poll() is None and time.monotonic() < deadline:
                    text = serial.read_text(errors="replace")
                    for marker, up in (("LINK_DOWN_REQUEST", False), ("LINK_UP_REQUEST", True)):
                        if marker in text and marker not in changed:
                            control = PowerProbe(qmp, work)
                            try: control.command("set_link", {"name": "testnic", "up": up})
                            finally: control.close()
                            changed.add(marker)
                    if "[network] DONE" in text or "KERNEL PANIC" in text: break
                    time.sleep(.2)
            finally:
                if process.poll() is None: iso_tools.qmp_quit(qmp, process)
    finally:
        stop.set()
        for server in servers: server.shutdown(); server.server_close()
        for thread in threads: thread.join(timeout=3)
    text = serial.read_text(errors="replace")
    lines = [line for line in text.splitlines() if "[network]" in line or "[e1000]" in line or "DHCP" in line]
    print("\n".join(lines))
    result = {"complete": args.gui or "[network] DONE failures=0" in text,
              "fixtures": counters, "qemu": command, "serial": str(serial),
              "kernel_sha256": hashlib.sha256((work / "iso/leonos/kernel.sys").read_bytes()).hexdigest(),
              "iso_sha256": hashlib.sha256(iso.read_bytes()).hexdigest()}
    (work / f"{args.nic}-smp{args.smp}.json").write_text(json.dumps(result, indent=2) + "\n")
    assert result["complete"] and all(counters.values()), result


if __name__ == "__main__":
    main()
