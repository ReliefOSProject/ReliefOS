#!/usr/bin/env python3
"""Regression for the generic ioctl(FIOCLEX/FIONCLEX) close-on-exec ABI.

Host stage (always runs, no virtual machine):
  * Linux UAPI header checks and dispatch-contract checks for the kernel
    source that answers ioctl(FIOCLEX/FIONCLEX),
  * the kernel descriptor-table unit test, built from the real
    kernel/reliefnt/kernel/reliefnt/syscall.c with ASan/UBSan,
  * the raw-syscall probe built with the ReliefOS musl SDK and executed on the
    host Linux kernel, which is the reference implementation of the ABI.

Guest stage (--guest): stage the same probe plus the unmodified static musl
CPython 3.15 build into an installer root, build the diagnostic ISO and boot
it under QEMU/KVM with serial console and syscall trace, then assert the guest
probe report and the real ``python3 /bin/hello.py`` run.
"""
from __future__ import annotations

import argparse
import hashlib
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LINUX_V612 = {
    # kernel/reliefnt/include/uapi/asm-generic/ioctls.h, Linux v6.12
    "FIOCLEX": 0x5451,
    "FIONCLEX": 0x5450,
}
FORMAT = "native x86-64 (i386/x32 deliberately out of scope)"


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def macro(text: str, name: str) -> int:
    match = re.search(rf"^#define {re.escape(name)}\s+(0x[0-9a-fA-F]+|\d+)", text, re.M)
    assert match, f"missing {name}"
    return int(match.group(1), 0)


def run(command: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run([str(item) for item in command], cwd=ROOT, check=True, **kwargs)


# --------------------------------------------------------------------------- #
# Host stage 1: Linux UAPI headers and kernel dispatch contract.
# --------------------------------------------------------------------------- #

def test_linux_headers() -> None:
    tty = read("kernel/reliefnt/include/uapi/linux/tty.h")
    fcntl = read("kernel/reliefnt/include/uapi/linux/fcntl.h")
    assert macro(tty, "FIOCLEX") == LINUX_V612["FIOCLEX"], "FIOCLEX must match Linux v6.12"
    assert macro(tty, "FIONCLEX") == LINUX_V612["FIONCLEX"], "FIONCLEX must match Linux v6.12"
    assert macro(fcntl, "LINUX_FD_CLOEXEC") == 1
    assert macro(fcntl, "LINUX_F_GETFD") == 1
    assert macro(fcntl, "LINUX_F_SETFD") == 2
    assert macro(fcntl, "LINUX_F_DUPFD_CLOEXEC") == 1030
    assert macro(fcntl, "LINUX_O_CLOEXEC") == 0x80000
    assert macro(fcntl, "LINUX_O_PATH") == 0x200000
    for name, value in LINUX_V612.items():
        print(f"  header {name}=0x{value:04x} matches Linux v6.12 {FORMAT}")


def test_kernel_dispatch_contract() -> None:
    syscall_c = read("kernel/reliefnt/kernel/reliefnt/syscall.c")
    sched_h = read("kernel/reliefnt/kernel/reliefnt/include/reliefnt/sched.h")
    # The generic requests must be resolved before any category/device routing.
    dispatch = syscall_c.index("static int64_t syscall_dispatch_regs(uint64_t number")
    generic = syscall_c.index("syscall_ioctl_descriptor_flags(sched_current_task(), a0")
    owned = syscall_c.index("if (syscall_fs_owns(number))", dispatch)
    assert dispatch < generic < owned, "FIOCLEX/FIONCLEX must precede device dispatch"
    path_rule = syscall_c.index("syscall_ioctl_resolve_fd(sched_current_task(), a0)")
    assert dispatch < path_rule < generic, "the O_PATH rule must run before generic ioctls"
    # Unknown requests keep the Linux generic error instead of ENOSYS.
    assert "return -RELIEFOS_ENOTTY;\n    }\n\n    return -RELIEFOS_ENOSYS;" in syscall_c
    # The signalfd branch must no longer carry a private implementation: the
    # generic handler answers FIOCLEX/FIONCLEX for it like for every other fd.
    signalfd = syscall_c.rindex("if (file && file->kind == TASK_FILE_KIND_SIGNALFD)")
    socket_dispatch = syscall_c.index("if (file && (file->flags & TASK_FILE_FLAG_SOCKET_UNIX))", signalfd)
    signalfd_block = syscall_c[signalfd:socket_dispatch]
    assert "FIOCLEX" not in signalfd_block and "FIONCLEX" not in signalfd_block
    nonblock = syscall_c.index("return syscall_ioctl_nonblock(sched_current_task()", dispatch)
    assert path_rule < nonblock < owned, "FIONBIO must use the shared generic VFS path"
    # fcntl(F_GETFD/F_SETFD) shares one storage with ioctl(FIOCLEX/FIONCLEX).
    assert "return task_fd_descriptor_flags(task, (int)a0);" in syscall_c
    assert "task_fd_set_descriptor_flags(task, (int)a0, (uint32_t)a2)" in syscall_c
    # The implicit stdio table must hold the flag, not answer success silently.
    assert "cloexec_stdio_mask" in syscall_c
    # O_PATH is FMODE_PATH, not an open flag, and must not collide with the
    # internal EPOLL bit used by the same word.
    assert macro(sched_h, "TASK_FILE_FLAG_EPOLL") == 0x00200000
    assert macro(sched_h, "TASK_FILE_FLAG_PATH") != macro(sched_h, "TASK_FILE_FLAG_EPOLL")
    assert "LINUX_O_PATH" in syscall_c and "TASK_FILE_FLAG_PATH" in syscall_c
    # Diagnostic guest hooks used by the ISO produced by --guest. Since the
    # stage-4 M5 migration the spawn authority is the desktop autospawn table
    # (exact-token "autospawn=<name>" parsed from /proc/cmdline), not the
    # kernel; the hook names must stay registered there.
    autospawn = read("userland/apps/desktop/autospawn.c")
    assert 'strncmp(token, "autospawn=", 10)' in autospawn, \
        "autospawn must match exact cmdline tokens, not substrings"
    for hook in ("ioctlcloexec", "python315"):
        assert f'{{"{hook}",' in autospawn, f"missing diagnostic hook {hook}"
    print("  kernel dispatch: generic FIOCLEX/FIONCLEX + O_PATH rule precede device dispatch")


# --------------------------------------------------------------------------- #
# Host stage 2: kernel descriptor-table unit test (real syscall.c).
# --------------------------------------------------------------------------- #

def test_kernel_descriptor_table(directory: Path) -> None:
    binary = directory / "ioctl-cloexec-table"
    sources = [
        "tools/tests/ioctl_cloexec_table_test.c",
        "kernel/reliefnt/kernel/reliefnt/sched/sched.c",
        "kernel/reliefnt/kernel/reliefnt/wait.c",
        "kernel/reliefnt/kernel/reliefnt/syscall_sysv_msg.c",
        "kernel/reliefnt/kernel/reliefnt/syscall_sysv_sem.c",
        "kernel/reliefnt/kernel/reliefnt/syscall_locks.c",
        "kernel/reliefnt/kernel/reliefnt/audio/core.c",
        "kernel/reliefnt/kernel/reliefnt/audio/pcm.c",
        "kernel/reliefnt/kernel/reliefnt/audio/device.c",
        "kernel/reliefnt/kernel/reliefnt/audio/alsa_control.c",
        "kernel/reliefnt/kernel/reliefnt/audio/mixer.c",
        "kernel/reliefnt/kernel/reliefnt/audio/oss.c",
    ]
    run([
        "clang", "-std=c11", "-g", "-O1", "-ffunction-sections", "-fdata-sections",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/kernel/reliefnt/include", "-Wl,--gc-sections",
        *sources, "-o", str(binary),
    ])
    result = subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=60,
                            stdout=subprocess.PIPE, text=True)
    print("  " + result.stdout.strip())
    run([str(binary), "--invalid-fd"], timeout=60)


# --------------------------------------------------------------------------- #
# Host stage 3: the raw-syscall probe on the host Linux reference kernel.
# --------------------------------------------------------------------------- #

def sdk_compiler(sdk: Path | None = None) -> Path:
    compiler = (sdk if sdk is not None else ROOT / "out/x86_64/release/sdk/reliefos-musl-sdk") / "bin/reliefos-musl-cc"
    if not compiler.is_file():
        raise SystemExit("missing SDK; run: make -j8 sdk")
    return compiler


def build_probe(binary: Path, sdk: Path | None = None) -> Path:
    binary.parent.mkdir(parents=True, exist_ok=True)
    run([sdk_compiler(sdk), "-static", "-O2", "-pthread", "-Wall", "-Wextra",
         "tools/tests/linux_ioctl_cloexec_test.c", "-o", str(binary)])
    return binary


def parse_probe(text: str) -> dict:
    summary = re.search(r"\[ioctl-clex\] DONE checks=(\d+) failures=(\d+) skips=(\d+)", text)
    assert summary, f"probe did not finish:\n{text[-4000:]}"
    results = {"checks": int(summary.group(1)), "failures": int(summary.group(2)),
               "skips": int(summary.group(3)),
               "pass": re.findall(r"\[ioctl-clex\] PASS (\S+)", text),
               "fail": re.findall(r"\[ioctl-clex\] FAIL ([^\s:]+)", text),
               "skip": re.findall(r"\[ioctl-clex\] SKIP ([^\s:]+)", text)}
    return results


# --------------------------------------------------------------------------- #
# Guest stage: stage, build the ISO and boot it under QEMU/KVM.
# --------------------------------------------------------------------------- #

GRUB_TEMPLATE = """set timeout=5
set default=0
insmod serial
insmod terminal
insmod fat
insmod iso9660
insmod multiboot2
insmod all_video
insmod font
insmod gfxterm
search --no-floppy --file /leonos-installer-iso.marker --set=root
loadfont /grub/fonts/leonos-unicode.pf2
set gfxmode=auto
set gfxpayload=keep
serial --unit=0 --speed=115200
terminal_input console serial
terminal_output gfxterm serial

menuentry "ReliefOS ioctl CLOEXEC regression" {
    multiboot2 /loader.elf root=/ log=serial mode=live syscall-trace=/opt/python/ autospawn=ioctlcloexec autospawn=python315
    module2 /leonos/kernel.sys leonos-kernel
    module2 /install/root.fat leonos-installer-root
    boot
}
"""

PROBE_GUEST_PATH = "/usr/lib/reliefos/tests/linux-ioctl-cloexec.elf"
PYTHON_GUEST_PATH = "/opt/python/bin/python3.15"
PYTHON_EXPECTED = (
    "Hello from Python 3 on ReliefOS!",
    "Numbers: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]",
    "Sum: 55",
    "Fibonacci: [0, 1, 1, 2, 3, 5, 8, 13, 21, 34]",
)


def debugfs(image: Path, command: str) -> None:
    run(["debugfs", "-w", "-R", command, str(image)], stdout=subprocess.DEVNULL,
        stderr=subprocess.STDOUT)


def stage_root(base: Path, probe: Path, image: Path) -> None:
    assert base.is_file(), f"missing installer root {base}"
    image.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(base, image)
    listing = subprocess.run(["debugfs", "-R", "ls -l /usr/share/examples/python", str(image)],
                             check=True, stdout=subprocess.PIPE, text=True).stdout
    assert "hello.py" in listing, f"{base} has no /usr/share/examples/python/hello.py"
    listing = subprocess.run(["debugfs", "-R", "ls -l /opt/python/bin", str(image)],
                             check=True, stdout=subprocess.PIPE, text=True).stdout
    assert "python3.15" in listing, f"{base} has no /opt/python/bin/python3.15"
    debugfs(image, "mkdir /usr/lib/leonos")
    debugfs(image, "mkdir /usr/lib/reliefos/tests")
    debugfs(image, f"write {probe} {PROBE_GUEST_PATH}")
    debugfs(image, f"set_inode_field {PROBE_GUEST_PATH} mode 0100755")
    debugfs(image, f"set_inode_field {PROBE_GUEST_PATH} uid 0")
    debugfs(image, f"set_inode_field {PROBE_GUEST_PATH} gid 0")
    listing = subprocess.run(["debugfs", "-R", "stat /usr/lib/reliefos/tests/linux-ioctl-cloexec.elf",
                              str(image)], check=True, stdout=subprocess.PIPE, text=True).stdout
    assert "Mode:  0755" in listing, listing
    print(f"  staged {PROBE_GUEST_PATH} and the CPython 3.15 tree in {image}")


def grub_efi_dir() -> str:
    """Use the pinned GRUB modules like build.py; the host GRUB build boots a
    different, unvalidated firmware image on this ISO layout."""
    repo = ROOT / "boot/grub_modules_x86_64-efi"
    if (repo / "modinfo.sh").is_file():
        return "boot/grub_modules_x86_64-efi"
    system = Path("/usr/lib/grub/x86_64-efi")
    assert (system / "modinfo.sh").is_file(), "no GRUB x86_64-efi modules available"
    return str(system)


def build_iso(root: Path, output: Path, config: Path, work: Path) -> Path:
    def artifact(*names: str) -> str:
        for name in names:
            if (ROOT / name).is_file():
                return name
        raise AssertionError(f"missing build artifact (tried {names}); build kernel and userland targets separately")

    loader = artifact("build/boot/loader.elf", "out/x86_64/release/generated/boot/loader.elf")
    kernel = artifact("build/system/kernel.sys", "out/x86_64/release/generated/system/kernel.sys")
    font = artifact("build/generated/grub/leonos-unicode.pf2", "out/x86_64/release/generated/grub/leonos-unicode.pf2")
    config.write_text(GRUB_TEMPLATE, encoding="ascii")
    assert "bootlog=1" not in config.read_text(encoding="ascii")
    run([
        sys.executable, "tools/make_installer_iso.py", "--out", str(output.relative_to(ROOT)),
        "--grub-efi-dir", grub_efi_dir(),
        "--stage", str((work / "iso").relative_to(ROOT)),
        "--boot-image", str((work / "efiboot.img").relative_to(ROOT)),
        "--loader", loader, "--kernel", kernel,
        "--installer-root", str(root.relative_to(ROOT)),
        "--grub-font", font,
        "--work-dir", str(work.relative_to(ROOT)),
        "--grub-config", str(config.relative_to(ROOT)),
    ], stdout=subprocess.DEVNULL)
    return output


def qmp_quit(sock: Path, process: subprocess.Popen) -> None:
    try:
        with socket.socket(socket.AF_UNIX) as connection:
            connection.settimeout(2)
            connection.connect(str(sock))
            connection.recv(65536)
            connection.sendall(b'{"execute":"qmp_capabilities"}\n')
            connection.recv(65536)
            connection.sendall(b'{"execute":"quit"}\n')
        process.wait(timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def terminal_python(qmp: Path, serial: Path, process: subprocess.Popen) -> None:
    from test_installer_window_qemu import Probe, wait_log

    probe = Probe(qmp, serial.parent)
    try:
        wait_log(serial, "[oobe.elf] starting first-run", process)
        time.sleep(10)
        probe.text("pythontest")
        probe.key("ret")
        time.sleep(8)
        probe.text("pythontest")
        probe.key("ret")
        time.sleep(5)
        probe.key("meta_l")
        time.sleep(1.5)
        probe.text("terminal")
        probe.key("ret")
        wait_log(serial, "terminal: PTY ready", process)
        time.sleep(4)
        before = len(serial.read_text(errors="replace"))
        probe.text("python3 /usr/share/examples/python/hello.py")
        probe.key("ret")
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            text = serial.read_text(errors="replace")[before:]
            launched = re.search(r"exec pid=(\d+) path=/opt/python/bin/python3\.15 pty=([1-9]\d*)", text)
            if launched:
                exited = re.search(rf"scheduler task exited pid={launched[1]} name=python3\.15 code=(\d+)", text)
                if exited:
                    assert exited[1] == "0", f"Terminal Python exited {exited[1]}"
                    time.sleep(2)
                    probe.frame("terminal-python")
                    print(f"  Terminal/PTY Python pid={launched[1]} exit=0; terminal-python.png")
                    return
            if process.poll() is not None:
                break
            time.sleep(0.2)
        probe.frame("terminal-python-failure")
        raise AssertionError("Terminal did not execute Python successfully")
    finally:
        probe.stream.close()
        probe.socket.close()


def boot_guest(iso: Path, serial: Path, timeout: float, terminal: bool = False) -> str:
    ovmf = Path("/usr/share/edk2/x64/OVMF.4m.fd")
    if not ovmf.is_file():
        raise SystemExit(f"missing {ovmf}; install the edk2 x86_64 firmware package")
    serial.parent.mkdir(parents=True, exist_ok=True)
    serial.write_text("")
    with tempfile.TemporaryDirectory(prefix="reliefos-ioctl-clex-") as directory:
        qmp = Path(directory) / "qmp.sock"
        command = [
            "qemu-system-x86_64", "-enable-kvm", "-cpu", "host", "-machine", "q35",
            "-m", "4096", "-smp", "2", "-bios", str(ovmf),
            "-display", "none", "-serial", f"file:{serial}",
            "-device", "VGA,xres=1280,yres=720", "-device", "qemu-xhci",
            "-device", "usb-tablet", "-netdev", "user,id=net0",
            "-device", "e1000,netdev=net0", "-cdrom", str(iso), "-boot", "d",
            "-qmp", f"unix:{qmp},server=on,wait=off", "-no-reboot", "-no-shutdown",
        ]
        process = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL)
        text = ""
        done = False
        python_done = None
        deadline = time.monotonic() + timeout
        try:
            while time.monotonic() < deadline:
                text = serial.read_text(errors="replace") if serial.exists() else ""
                done = "[ioctl-clex] DONE" in text
                python_done = re.search(r"name=python3\.15 code=\d+", text)
                if done and python_done:
                    break
                if "KERNEL PANIC" in text or process.poll() is not None:
                    break
                time.sleep(0.5)
            if terminal and done and python_done:
                terminal_python(qmp, serial, process)
                text = serial.read_text(errors="replace")
        finally:
            if process.poll() is None:
                qmp_quit(qmp, process)
        if not text:
            text = serial.read_text(errors="replace")
    return text


def test_guest_iso(args: argparse.Namespace) -> None:
    work = ROOT / "build/ioctl-cloexec"
    work.mkdir(parents=True, exist_ok=True)
    probe = build_probe(work / "linux-ioctl-cloexec.elf", args.sdk)
    root = args.root if args.root.is_absolute() else ROOT / args.root
    image = work / "root.ext2"
    stage_root(root, probe, image)
    iso = build_iso(image, work / "reliefos-ioctl-cloexec.iso", work / "grub-ioctl-cloexec.cfg", work)
    serial = work / "guest-serial.log"
    text = boot_guest(iso, serial, args.timeout, args.terminal)

    assert "KERNEL PANIC" not in text, "guest kernel panicked"
    results = parse_probe(text)
    assert results["failures"] == 0, f"guest probe failures={results['failures']}: {results['fail']}"
    assert results["skips"] == 0, f"guest probe skipped cases: {results['skip']}"
    required = {"ioctl-invalid-fd-precedence", "type-epoll", "epoll-add-wait",
                "exec-closes-epoll", "epoll-dup-mod-del"}
    assert required <= set(results["pass"]), "guest did not exercise both rework regressions"
    assert "DONE checks=" in text
    for expected in PYTHON_EXPECTED:
        assert expected in text, f"python output missing: {expected!r}"
    match = re.search(r"scheduler task exited pid=(\d+) name=python3\.15 code=(\d+)", text)
    assert match, "the guest never exited the Python 3.15 task"
    assert match.group(2) == "0", f"python exited with code {match.group(2)}"
    assert "can't open file" not in text and "Function not implemented" not in text
    ioctl_calls = re.findall(r"\[syscall-trace\] pid=\d+ nr=16 args=[0-9a-f]+,5451,.*?\n", text)
    ok_calls = [call for call in ioctl_calls if "result=0" in text.split(call, 1)[1][:200]]
    assert ok_calls, "no successful ioctl(FIOCLEX) syscall was traced for Python"
    print(f"  guest: ioctl-clex checks={results['checks']} failures=0 skips={results['skips']}; "
          f"python pid={match.group(1)} exit=0; {len(ioctl_calls)} traced FIOCLEX call(s)")
    print(f"  guest serial log: {serial.relative_to(ROOT)}")
    print(f"  test ISO: {iso.relative_to(ROOT)}")
    return text, iso, serial


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


GUEST_BEGIN = "--- guest evidence (recorded by the last --guest run"
GUEST_END = "--- end guest evidence ---"


def preserved_guest_evidence(path: Path) -> str:
    """Return the guest block of an earlier run so host-only runs keep it."""
    if not path.is_file():
        return ""
    text = path.read_text(encoding="utf-8")
    start = text.find(GUEST_BEGIN)
    end = text.find(GUEST_END)
    if start < 0 or end <= start:
        return ""
    return text[start:end + len(GUEST_END)]


def write_evidence(host_output: str, host: dict, guest_text: str | None,
                   iso: Path | None, serial: Path | None, *,
                   output: Path | None = None, kernel: Path | None = None) -> Path:
    """Record the four evidence levels separately: source, host, guest, pending."""
    path = output if output is not None else ROOT / "build/ioctl-cloexec/evidence.txt"
    kernel_path = kernel if kernel is not None else ROOT / "out/x86_64/release/generated/system/kernel.sys"
    path.parent.mkdir(parents=True, exist_ok=True)
    commit = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT,
                            stdout=subprocess.PIPE, text=True, check=True).stdout.strip()
    lines = [
        "ioctl(FIOCLEX/FIONCLEX) close-on-exec regression evidence",
        f"format: {FORMAT}",
        f"commit: {commit}",
        f"kernel.sys sha256: {digest(kernel_path)}",
        f"probe sha256: {digest(ROOT / 'build/ioctl-cloexec/linux-ioctl-cloexec.elf')}",
        "",
        f"[host] host Linux raw-syscall reference: checks={host['checks']} "
        f"failures={host['failures']} skips={host['skips']}",
        "[host] kernel descriptor-table unit test (real syscall.c, ASan/UBSan): PASS",
        "",
        "--- host reference probe output ---",
        host_output.strip(),
    ]
    kernel_hash = digest(kernel_path)
    previous_guest = preserved_guest_evidence(path)
    if guest_text is not None and iso is not None and serial is not None:
        guest = parse_probe(guest_text)
        lines += [
            "",
            f"{GUEST_BEGIN} for kernel.sys sha256 {kernel_hash}) ---",
            f"[guest] QEMU/KVM ISO: {iso.relative_to(ROOT)} sha256: {digest(iso)}",
            f"[guest] serial log: {serial.relative_to(ROOT)}",
            f"[guest] probe: checks={guest['checks']} failures={guest['failures']} "
            f"skips={guest['skips']}",
            "",
            "--- guest probe and Python output (serial) ---",
        ]
        serial_lines = guest_text.splitlines()
        for index, line in enumerate(serial_lines):
            if any(marker in line for marker in (
                    "[ioctl-clex]", "Hello from Python", "Numbers:", "Sum:", "Fibonacci:",
                    "name=python3.15 code=", "[desktop] autospawn ioctlcloexec",
                    "[desktop] autospawn python315", "5451")):
                lines.append(line.strip())
                # Keep the syscall-trace result line that follows a FIOCLEX call.
                if "5451" in line and index + 1 < len(serial_lines):
                    lines.append(serial_lines[index + 1].strip())
        lines += [GUEST_END]
    elif previous_guest:
        # A host-only run must not erase the guest evidence recorded earlier.
        lines += ["", previous_guest.rstrip("\n")]
    else:
        lines += ["", "[guest] not run in this invocation (use --guest)."]
    lines += [
        "",
        "[pending] VMware Workstation run not performed in this checkpoint.",
        "[pending] i386/x32 compat ioctl and the remaining per-device ioctl "
        "requests are out of scope here.",
    ]
    with path.open("x" if output is not None else "w", encoding="utf-8") as evidence:
        evidence.write("\n".join(lines) + "\n")
    return path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, help="SDK prefix produced by the selected O= build")
    parser.add_argument("--kernel", type=Path, help="kernel.sys used for evidence hashing")
    parser.add_argument("--evidence", type=Path, help="create a new evidence file without overwriting it")
    parser.add_argument("--guest", action="store_true",
                        help="also build the diagnostic ISO and boot it under QEMU/KVM")
    parser.add_argument("--terminal", action="store_true",
                        help="also exercise Python through desktop Terminal (requires --guest)")
    parser.add_argument("--root", type=Path,
                        default=Path("build/python315-stdlib-test/root.ext2"),
                        help="installer root ext2 image that already contains CPython 3.15")
    parser.add_argument("--timeout", type=float, default=300.0)
    args = parser.parse_args()
    if args.terminal and not args.guest:
        parser.error("--terminal requires --guest")

    print(f"ioctl close-on-exec regression ({FORMAT})")
    test_linux_headers()
    test_kernel_dispatch_contract()
    with tempfile.TemporaryDirectory(prefix="reliefos-ioctl-cloexec-") as directory:
        test_kernel_descriptor_table(Path(directory))
        probe = build_probe(ROOT / "build/ioctl-cloexec/linux-ioctl-cloexec.elf", args.sdk)
        host_output = subprocess.run([str(probe)], cwd=ROOT, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True,
                                     timeout=120).stdout
        host = parse_probe(host_output)
        print(host_output.strip())
        assert host["failures"] == 0, f"host Linux reference reported {host['failures']} failures"
        print(f"  host Linux reference: {host['checks']} checks, 0 failures, {host['skips']} skips")
    guest_text = None
    iso = None
    serial = None
    if args.guest:
        guest_text, iso, serial = test_guest_iso(args)
    evidence = write_evidence(host_output, host, guest_text, iso, serial,
                              output=args.evidence, kernel=args.kernel)
    print(f"  evidence: {evidence.relative_to(ROOT)}")
    print("PASS ioctl close-on-exec: host Linux reference and kernel unit test; "
          + ("guest ISO verified" if args.guest else "guest ISO not requested"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
