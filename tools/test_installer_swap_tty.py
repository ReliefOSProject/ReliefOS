#!/usr/bin/env python3
"""Run real ncurses navigation and fresh/update wizard regressions on a PTY."""
import argparse
import fcntl
import os
from pathlib import Path
import pty
import select
import signal
import struct
import subprocess
import termios
import time
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def compile_fixture(binary):
    sources = ["installer_tui.c", "installer_tui_job.c", "installer_tui_partitions.c", "installer_tty.c", "model.c",
               "installer_capacity.c", "installer_partitions.c", "installer_update_progress.c", "apache_license.S"]
    subprocess.run([
        "cc", "-std=gnu11", "-D_GNU_SOURCE", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections,--wrap=open,--wrap=doupdate",
        "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi",
        "-idirafter", "userland/runtime/include", "tools/tests/installer_tty_input_test.c",
        *[f"userland/apps/installer/{name}" for name in sources],
        f'-DINSTALLER_LICENSE_PATH="{ROOT / "LICENSE"}"', "-lncursesw", "-o", str(binary),
    ], cwd=ROOT, check=True)


class Session:
    def __init__(self, binary, output, name, *, partitions=False):
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
        self.attributes = termios.tcgetattr(slave)
        self.slave = slave
        session_output = Path(tempfile.mkdtemp(prefix=name + "-", dir=output))
        self.result = session_output / "result"
        self.log = session_output / "installer.log"
        self.screen = session_output / "screen.txt"
        self.operation = session_output / "operation.txt"
        self.data = bytearray()
        self.process = subprocess.Popen(
            [binary, "--interactive"], stdin=slave, stdout=slave, stderr=slave,
            env={**os.environ, "TERM": "xterm", "LANG": "C.UTF-8",
                 "INSTALLER_TEST_LOG": str(self.log), "INSTALLER_TEST_RESULT": str(self.result),
                 "INSTALLER_TEST_SCREEN": str(self.screen),
                 "INSTALLER_TEST_OPERATION": str(self.operation),
                 **({"INSTALLER_TEST_PARTITIONS": "1"} if partitions else {}),
                 "ASAN_OPTIONS": "detect_leaks=0"}, cwd=ROOT,
        )
        self.drain()

    def drain(self, delay=0.12):
        until = time.monotonic() + delay
        while time.monotonic() < until:
            ready, _, _ = select.select([self.master], [], [], max(0, until - time.monotonic()))
            if ready:
                self.data.extend(os.read(self.master, 65536))

    def key(self, value):
        os.write(self.master, value)
        self.drain()

    def next(self):
        self.key(b"\x1bOR")  # xterm F3

    def back(self):
        self.key(b"\x1bOQ")  # xterm F2

    def resize(self, rows, columns):
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
        self.process.send_signal(signal.SIGWINCH)
        self.drain(0.35)

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            self.drain(0.3)
        self.process.wait(timeout=10)
        assert termios.tcgetattr(self.slave) == self.attributes, "terminal attributes were not restored"
        os.close(self.master)
        os.close(self.slave)
        assert b"AddressSanitizer" not in self.data and b"runtime error:" not in self.data, self.data[-2000:]


def wizard_tests(binary, output):
    fresh = Session(binary, output, "fresh")
    try:
        for _ in range(6):  # language, license, welcome, mode, disk, bootloaders
            fresh.next()
        fresh.next()  # automatic plan -> account options
        for value in (b"alice", b"user-secret", b"user-secret", b"root-secret", b"root-secret"):
            fresh.key(value + b"\t")
        fresh.key(b" ")  # console-only
        fresh.key(b"\t ")  # enable swap
        fresh.key(b"\t\x152049")  # replace suggestion
        fresh.next()
        fresh.next()  # missing confirmation must leave disk untouched
        assert not fresh.result.exists()
        fresh.back()  # account values survive returning from review
        fresh.next()
        fresh.key(b"INSTALL")
        fresh.next()
        fresh.drain(0.5)
        assert fresh.result.read_text() == "user=alice xorg=0 bios=1 uefi=1 swap=1 size=2049\n"
        assert b"user-secret" not in fresh.data and b"root-secret" not in fresh.data
        assert "user-secret" not in fresh.log.read_text() and "root-secret" not in fresh.log.read_text()
        fresh.next()  # exit completion page
        fresh.process.wait(timeout=10)
        assert fresh.process.returncode == 0
    finally:
        fresh.close()

    update = Session(binary, output, "update")
    try:
        for _ in range(3):
            update.next()
        update.key(b"\t ")
        update.next()  # update mode -> disk
        update.next()  # inspect -> update review
        update.drain(0.4)
        assert not update.result.exists()
        update.key(b" ")  # disable BIOS, preserve UEFI
        update.next()
        update.drain(0.4)
        assert "bios=0 uefi=1" in update.result.read_text()
        update.next()
        update.process.wait(timeout=10)
        assert update.process.returncode == 0
    finally:
        update.close()
    print("PASS PTY fresh install/update, confirmation, Back, desktop/swap/firmware choices and terminal restoration")


def partition_table_tests(binary, output):
    """Catch missing columns, lost row-to-device mapping and accidental writes."""
    session = Session(binary, output, "partition-table", partitions=True)
    try:
        for _ in range(6):
            session.next()
        session.drain(0.3)
        automatic = session.screen.read_text()
        assert all(label in automatic for label in ("Partition", "Filesystem", "RELIEFOS_ROOT")), automatic
        assert not session.operation.exists() and not session.result.exists()
        session.key(b"\t ")  # manual layout
        session.key(b"\t")  # focus the table
        session.key(b"\x1bOH")  # Home -> first physical partition, GPT slot 1
        session.key(b"\x1bOB")  # ESP, GPT slot 4
        session.key(b"\x1bOB")  # unallocated range must never select a partition
        session.key(b"\t")
        # Refresh, Apply automatic layout, Initialize GPT, Create, Resize, Rename.
        for _ in range(5):
            session.key(b"\t")
        session.key(b"\r")
        session.drain(0.25)
        assert "Select a partition first." in session.screen.read_text()
        assert not session.operation.exists()
        session.next()  # close the message
        for _ in range(6):
            session.key(b"\x1b[Z")  # Shift+Tab back to the table
        session.key(b"\x1bOF")  # End -> actual root, GPT slot 7
        for _ in range(6):
            session.key(b"\t")
        session.key(b"\r")
        session.drain(0.3)
        assert "Partition name" in session.screen.read_text()
        session.key(b"\x15Renamed root")
        session.next()
        session.drain(0.4)
        assert session.operation.read_text() == "slot=7 name=Renamed root\n"
        session.resize(20, 60)
        session.key(b"\x1b[Z" * 6)
        session.key(b"\x1bOH")
        session.drain(0.3)
        session.key(b"\x1b[6~")  # PgDn remains within the table
        session.drain(0.2)
        assert "Filesystem" in session.screen.read_text()
        assert not session.result.exists()
        session.resize(36, 110)
        session.key(b"\x1bOH")
        assert "/dev/fixture2" in session.screen.read_text()
    finally:
        session.close()
    print("PASS PTY partition table, preview, free-space selection, GPT-slot mapping and terminal resize")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    output = args.output or Path(tempfile.mkdtemp(prefix="installer-tui-", dir=os.environ.get("TMPDIR")))
    output.mkdir(parents=True, exist_ok=True)
    output = output.resolve()
    binary = output / "tui"
    compile_fixture(binary)
    subprocess.run([binary], cwd=ROOT, check=True, timeout=20)
    wizard_tests(binary, output)
    partition_table_tests(binary, output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
