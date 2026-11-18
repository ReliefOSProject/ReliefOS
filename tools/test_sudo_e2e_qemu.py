#!/usr/bin/env python3
"""End-to-end verification of sudo/su elevation on an installed ReliefOS disk.

Boots a scratch disk installed from the current installer ISO, logs into the
desktop as the ordinary account, and drives the Terminal through the QMP
keyboard. This is the only check that can prove the privileged broker really
changes an identity: the host suites prove the authorization decisions, but not
that the guest performs a setuid-free elevation through authd.

Keyboard fidelity matters here. `sendkey` needs an explicit QEMU key name for
every punctuation character, so this module supplies the full map instead of
relying on the base helper's partial one; an unmapped character silently splits
the command line and would make the test blame the shell.

The oracle is the screenshot, not the serial log: Terminal renders the PTY
output into its window and never writes it to the serial console, so a serial
scrape would see nothing and a screenshot shows the actual result. Each step
therefore types a command, waits, and captures a frame; the frames are the
evidence this run leaves behind.

Ordering is deliberate: the first frame must show the ordinary account,
otherwise nothing below proves anything about privilege. The password is typed
at the guest terminal rather than piped, because sudo and su read the terminal
directly and that is also what makes the echo-suppression claim meaningful.
"""
from __future__ import annotations

import argparse
import re
import time
from pathlib import Path

from test_installer_accounts_qemu import (
    Probe as BaseProbe, install_tty, wait_log, USER_PASSWORD, ROOT_PASSWORD, boot,
)

ROOT = Path(__file__).resolve().parents[1]

# The TTY installer provisions the two accounts with different passwords: the
# ordinary account gets the long constant, root gets a short one. This script
# installs with `install_tty` from the sibling module, whose answers hardcode
# root's password, so the two must not be confused -- using the alice password
# for root produces a *correct* authentication failure.
ROOT_PW = "r"

PUNCTUATION = {
    " ": "spc", ".": "dot", "/": "slash", "\\": "backslash", "=": "equal",
    "-": "minus", "_": "shift-minus", ":": "shift-semicolon",
    "|": "shift-backslash", "&": "shift-7", ";": "semicolon",
    "(": "shift-9", ")": "shift-0", ">": "shift-dot", "$": "shift-4",
    "?": "shift-slash", "'": "apostrophe", '"': "shift-apostrophe",
    "!": "shift-1", "[": "bracket_left", "]": "bracket_right",
    "*": "shift-8", "#": "shift-3", "%": "shift-5", "@": "shift-2",
    "^": "shift-6", "+": "shift-equal", "<": "shift-comma", ",": "comma",
}


class Probe(BaseProbe):
    """Terminal probe that can type shell punctuation reliably."""

    def text(self, text):
        for character in text:
            if "A" <= character <= "Z":
                self.key(f"shift-{character.lower()}")
            elif "a" <= character <= "z" or "0" <= character <= "9":
                self.key(character)
            else:
                key = PUNCTUATION.get(character)
                if key is None:
                    raise AssertionError(
                        f"no QEMU key mapping for {character!r}; "
                        "add it to PUNCTUATION rather than dropping it")
                self.key(key)
        # The keyboard and the pointer travel through separate guest queues.
        time.sleep(0.35)


def type_line(probe, command, settle=4.0):
    """Type one command and give the guest time to render its output."""
    probe.text(command)
    probe.key("ret")
    time.sleep(settle)


def type_password(probe, serial, process, marker, password, timeout=120):
    """Wait for the guest's password prompt, then answer at the keyboard.

    Waiting matters: typing before the prompt is echoed sends the secret to the
    shell instead of to sudo, which reads as an authentication failure.
    """
    wait_log(serial, marker, process)
    time.sleep(1)
    probe.text(password)
    probe.key("ret")
    time.sleep(6)


def login_desktop_terminal(probe, serial, process):
    """Reach an X11 st window with a real PTY as the ordinary user."""
    wait_log(serial, "[login.elf] starting login UI", process)
    time.sleep(5)
    probe.frame("login")
    probe.key("down")
    probe.text(USER_PASSWORD)
    probe.key("ret")
    wait_log(serial, "name=login.elf code=0", process)
    time.sleep(6)
    probe.key("meta_l")
    time.sleep(1.5)
    probe.text("terminal")
    probe.key("ret")
    time.sleep(3)
    type_line(probe, "echo ST_PTY_READY >/dev/ttyS0", settle=0.5)
    wait_log(serial, "ST_PTY_READY", process)
    probe.frame("terminal-ready")


def run_elevation_checks(probe, serial, process, output):
    """Drive the interactive checks and leave one screenshot per step.

    Verification of the frames is done by reading them: the shells' output never
    reaches the serial console.
    """
    frames = []

    # 1. The starting identity. This must be the ordinary account.
    type_line(probe, "grep Uid /proc/self/status")
    frames.append(probe.frame("step1-unprivileged"))

    # 2. The privilege boundary: this read must be denied.
    type_line(probe, "cat /root/.leon-e2e; echo direct-read-status=$?")
    frames.append(probe.frame("step2-boundary-denied"))

    # 3. Without a password there is no window, so sudo must refuse.
    type_line(probe, "sudo -n grep Uid /proc/self/status; echo no-prompt-status=$?")
    frames.append(probe.frame("step3-no-password-refused"))

    # 4. A wrong password must be refused and must not leave a window.
    probe.text("sudo grep Uid /proc/self/status")
    probe.key("ret")
    type_password(probe, serial, process, "password for root",
                  "definitely-not-the-password")
    frames.append(probe.frame("step4-wrong-password"))

    # 5. The real elevation.
    probe.text("sudo grep Uid /proc/self/status")
    probe.key("ret")
    type_password(probe, serial, process, "password for root", ROOT_PW)
    frames.append(probe.frame("step5-sudo-root"))

    # 6. The previously denied path is reachable through the window.
    type_line(probe, "sudo ls /root; echo root-list-status=$?")
    frames.append(probe.frame("step6-root-readable"))

    # 7. sudo -k must discard the window.
    type_line(probe, "sudo -k; sudo -n true; echo after-kill-status=$?")
    frames.append(probe.frame("step7-kill"))

    # 8. su must reach a root identity through the switch path.
    probe.text("su -c 'grep Uid /proc/self/status'")
    probe.key("ret")
    type_password(probe, serial, process, "assword", ROOT_PW)
    frames.append(probe.frame("step8-su-root"))

    print(f"PASS(driver): {len(frames)} frames captured for review: {output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", type=Path,
                        default=ROOT / "build/images/reliefos-installer.iso")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--skip-install", action="store_true",
                        help="Only boot the existing scratch disk")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    disk = output / "scratch.raw"
    if not args.skip_install:
        if disk.exists():
            disk.unlink()
        with disk.open("ab") as file:
            file.truncate(8 * 1024**3)
        with boot(output / "install", disk, args.iso.resolve()) as session:
            install_tty(*session, "none")
    if not disk.is_file():
        parser.error("no scratch disk; run without --skip-install first")
    with boot(output / "desktop", disk) as session:
        probe, serial, process = session
        login_desktop_terminal(probe, serial, process)
        run_elevation_checks(probe, serial, process, output)


if __name__ == "__main__":
    main()
