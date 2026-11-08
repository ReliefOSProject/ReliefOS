#!/usr/bin/env python3
"""Exercise issue #28 on an exclusive disk copy (requires Pillow and Tesseract)."""
import argparse
from pathlib import Path
import subprocess
import time

from test_installer_accounts_qemu import boot, next_page, previous_page

ROOT = Path(__file__).resolve().parents[1]


def page_text(probe, name):
    frame = probe.frame(name)
    if frame.width < 640 or frame.height < 480:
        return ""
    crop = probe.output / "content.png"
    if probe._detect_installer_sidebar(frame) is None:
        return ""
    content = frame.crop((max(0, probe.content_left - 8), probe.sidebar_top + 8,
                          min(frame.width, probe.content_left + 790),
                          min(frame.height, probe.sidebar_top + 600)))
    content.resize((content.width * 2, content.height * 2)).save(crop)
    return subprocess.check_output(["tesseract", str(crop), "stdout", "--psm", "6"],
                                   stderr=subprocess.DEVNULL, text=True)


def wait_page(probe, process, text, timeout=90):
    deadline = time.monotonic() + timeout
    previous = ""
    while time.monotonic() < deadline:
        actual = page_text(probe, "progress")
        if actual != previous:
            print(actual, flush=True)
            previous = actual
        if text in actual:
            return
        if "Failed" in actual or process.poll() is not None:
            raise AssertionError(f"Installer failed while waiting for {text}: {actual}")
        time.sleep(2)
    raise AssertionError(f"Timed out waiting for {text}: {previous}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disk", type=Path, required=True)
    parser.add_argument("--iso", type=Path, default=ROOT / "out/x86_64/release/images/reliefos-installer.iso")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    disk = output / "scratch.raw"
    if disk.exists():
        parser.error("scratch.raw already exists; use a new output directory")
    subprocess.run(["cp", "--reflink=auto", "--sparse=always", args.disk.resolve(), disk], check=True)
    if disk.stat().st_size < 4 * 1024**3:
        subprocess.run(["qemu-img", "resize", "-f", "raw", disk, "4G"], check=True)
    failures = []
    with boot(output, disk, args.iso.resolve()) as (probe, serial, process):
        wait_page(probe, process, "Select Language")
        probe.frame("language")
        # The FLTK wizard no longer has the Motif-only Style step, so Mode is
        # reached after three transitions from Language.
        for _ in range(3):
            next_page(probe)
        probe.click(*probe.mode_option_point(update=True))
        next_page(probe)
        next_page(probe)
        wait_page(probe, process, "Review system update", timeout=600)
        probe.frame("update-review")
        # Leave the inspected update unsubmitted, return to Mode, and carry
        # the same disk through fresh installation with independent defaults.
        previous_page(probe, 4)
        previous_page(probe, 3)
        probe.click(*probe.mode_option_point(update=False))
        next_page(probe, 4)
        probe.click(*probe.disk_selection_point())
        next_page(probe, 5)
        next_page(probe, 6)
        next_page(probe, 7)
        for index, value in enumerate(("alice", "password", "password", "rootpass", "rootpass")):
            probe.click(*probe.account_field_point(index))
            probe.text(value)
        next_page(probe, 8)
        probe.click(*probe.confirm_field_point())
        probe.text("INSTALL")
        probe.frame("fresh-confirm-typed")
        next_page(probe, 9)
        try:
            wait_page(probe, process, "Installation Complete", timeout=2400)
        except AssertionError as error:
            failures.append(str(error))
        if failures:
            raise AssertionError("; ".join(failures))
        print(f"PASS inspected update-to-fresh installation; {serial}", flush=True)


if __name__ == "__main__":
    main()
