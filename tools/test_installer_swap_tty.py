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
    sources = ["installer_tui.c", "installer_tui_job.c", "installer_job.c", "installer_tui_partitions.c", "installer_tty.c", "model.c",
               "installer_capacity.c", "installer_partitions.c", "installer_update_progress.c", "apache_license.S"]
    subprocess.run([
        "cc", "-std=gnu11", "-D_GNU_SOURCE", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections,--wrap=open,--wrap=doupdate,--wrap=installer_partition_reset,--wrap=installer_partition_select_disk,--wrap=installer_partition_select_mode,--wrap=installer_partition_changes,--wrap=installer_partition_error,--wrap=installer_partition_disk_error",
        "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi",
        "-idirafter", "userland/runtime/include", "tools/tests/installer_tty_input_test.c",
        *[f"userland/apps/installer/{name}" for name in sources],
        f'-DINSTALLER_LICENSE_PATH="{ROOT / "LICENSE"}"', "-lncursesw", "-o", str(binary),
    ], cwd=ROOT, check=True)


def fixture_environment(directory):
    # ncurses/libtinfo retains two terminfo reallocations in _nc_doalloc after
    # delscreen(). Suppress only that external cache; installer leaks remain
    # checked by LSan, alongside all ASan/UBSan checks.
    suppression = Path(directory).resolve() / "ncurses.lsan"
    suppression.write_text("leak:_nc_doalloc\n")
    options = os.environ.get("LSAN_OPTIONS", "")
    return {**os.environ, "LSAN_OPTIONS":
            options + (":" if options else "") + "suppressions=" + str(suppression)}


class Session:
    def __init__(self, binary, output, name, *, partitions=False, fail_install=False, fail_summary=False):
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
            env={**fixture_environment(session_output), "TERM": "xterm", "LANG": "C.UTF-8",
                 "INSTALLER_TEST_LOG": str(self.log), "INSTALLER_TEST_RESULT": str(self.result),
                 "INSTALLER_TEST_SCREEN": str(self.screen),
                 "INSTALLER_TEST_OPERATION": str(self.operation),
                 **({"INSTALLER_TEST_PARTITIONS": "1"} if partitions else {}),
                 **({"INSTALLER_TEST_INSTALL_FAILURE": "1"} if fail_install else {}),
                 **({"INSTALLER_TEST_SUMMARY_FAILURE": "1"} if fail_summary else {}),
                 }, cwd=ROOT,
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
        for _ in range(5):  # language, license, mode, disk, bootloaders
            fresh.next()
        fresh.next()  # automatic plan -> system options
        fresh.key(b"\t\t ")  # select the third desktop option: console-only
        fresh.key(b"\t ")  # enable swap
        fresh.key(b"\t\x152049")  # replace suggestion
        fresh.next()  # system options -> accounts
        for value in (b"alice", b"user-secret", b"user-secret", b"root-secret", b"root-secret"):
            fresh.key(value + b"\t")
        fresh.next()
        fresh.next()  # missing confirmation must leave disk untouched
        assert not fresh.result.exists()
        fresh.back()  # account values survive returning from review
        fresh.back()  # system choices also survive
        assert "System options" in fresh.screen.read_text()
        fresh.next()
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
        for _ in range(2):
            update.next()
        update.key(b"\t ")
        update.next()  # update mode -> disk
        update.next()  # inspect, then edit firmware on the target page
        update.drain(0.4)
        assert not update.result.exists()
        update.key(b"\t\t\t\t ")  # disk, refresh, root, ESP, BIOS; disable BIOS
        update.next()  # read-only review
        update.back()  # preserve checked target and edited firmware
        assert "Check system" not in update.screen.read_text()
        update.next()
        update.next()
        update.drain(0.4)
        assert "bios=0 uefi=1" in update.result.read_text()
        update.next()
        update.process.wait(timeout=10)
        assert update.process.returncode == 0
    finally:
        update.close()
    print("PASS PTY fresh install/update, confirmation, Back, desktop/swap/firmware choices and terminal restoration")


def failed_install_test(binary, output):
    """An error returns to reviewed configuration rather than replaying writes."""
    session = Session(binary, output, "failed-install", fail_install=True)
    try:
        for _ in range(7):
            session.next()  # through system options to accounts
        for value in (b"alice", b"user-secret", b"user-secret", b"root-secret", b"root-secret"):
            session.key(value + b"\t")
        session.next()
        session.key(b"INSTALL")
        session.next()
        session.drain(0.4)
        assert "Review settings" in session.screen.read_text()
        assert "Installation failed" in session.screen.read_text()
        session.next()
        assert "Target disk" in session.screen.read_text()
        assert not session.result.exists()
        for _ in range(4):
            session.next()
        assert "Accounts" in session.screen.read_text()
        session.next()  # retained accounts -> review
        session.next()  # INSTALL was cleared after failure
        assert "Review installation" in session.screen.read_text()
        assert not session.result.exists()
    finally:
        session.close()
    print("PASS failed-install review, retained account values and renewed destructive confirmation")


def incomplete_summary_test(binary, output):
    """Never apply a plan whose complete destructive changes could not be shown."""
    session = Session(binary, output, "incomplete-summary", fail_summary=True)
    try:
        for _ in range(7):
            session.next()
        for value in (b"alice", b"user-secret", b"user-secret", b"root-secret", b"root-secret"):
            session.key(value + b"\t")
        session.next()
        assert "The disk plan could not be read." in session.screen.read_text()
        session.key(b"INSTALL")
        session.next()
        assert "Review installation" in session.screen.read_text()
        assert not session.result.exists()
    finally:
        session.close()
    print("PASS incomplete plan summary blocks installation")


def partition_table_tests(binary, output):
    """Catch missing columns, lost row-to-device mapping and accidental writes."""
    session = Session(binary, output, "partition-table", partitions=True)
    try:
        for _ in range(5):
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
        # Refresh, Discard changes, Initialize GPT, Create, Resize, Rename.
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
        session.key(b"\t\t\r")  # table -> Refresh -> Discard changes
        session.next()  # confirm discarding the draft
        session.drain(0.3)
        assert "Installation layout - changes remain pending" in session.screen.read_text()
        assert "Pending layout - written only after" not in session.screen.read_text()
        assert not session.result.exists()
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
    subprocess.run([binary], cwd=ROOT, check=True, timeout=20, env=fixture_environment(output))
    wizard_tests(binary, output)
    failed_install_test(binary, output)
    incomplete_summary_test(binary, output)
    partition_table_tests(binary, output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
