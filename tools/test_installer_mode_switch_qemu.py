#!/usr/bin/env python3
"""Exercise issue #28 on an exclusive disk copy (requires Pillow and Tesseract)."""
import argparse
from difflib import SequenceMatcher
import os
from pathlib import Path
import subprocess
import re
import time

from test_installer_accounts_qemu import (
    boot, next_page, previous_page, select_installation_mode,
)

ROOT = Path(__file__).resolve().parents[1]


def page_text(probe, name):
    frame = probe.frame(name)
    if frame.width < 640 or frame.height < 480:
        return ""
    crop = probe.output / "content.png"
    if probe._detect_installer_sidebar(frame) is None:
        return ""
    content = frame.crop((max(0, probe.content_left - 8), probe.sidebar_top + 8,
                          min(frame.width, probe.window_right),
                          min(frame.height, probe.sidebar_bottom)))
    content.resize((content.width * 2, content.height * 2)).save(crop)
    text = subprocess.check_output(["tesseract", str(crop), "stdout", "--psm", "6"],
                                   stderr=subprocess.DEVNULL, text=True, timeout=15,
                                   env={**os.environ, "OMP_THREAD_LIMIT": "1"})
    # Page-sized thresholding can omit the native gray footer button. Its
    # actual visible label is the disk inspection's completion gate.
    x1, y1, x2, y2 = probe.footer_button_box()
    footer = probe.output / "footer-content.png"
    frame.crop((x1 - 2, y1, x2, y2 + 2)).resize((600, 160)).save(footer)
    text += subprocess.check_output(["tesseract", str(footer), "stdout", "--psm", "7"],
                                    stderr=subprocess.DEVNULL, text=True, timeout=10,
                                    env={**os.environ, "OMP_THREAD_LIMIT": "1"})
    return text


def wait_page(probe, process, text, timeout=90):
    deadline = time.monotonic() + timeout
    previous = ""
    while time.monotonic() < deadline:
        actual = page_text(probe, "progress")
        if actual != previous:
            print(actual, flush=True)
            previous = actual
        words = re.findall(r"[a-z]+", actual.lower())
        expected = re.sub(r"[^a-z]+", "", text.lower())
        review_update = expected == "reviewupdate" and any(
            SequenceMatcher(None, expected, "".join(words[index:index + 2])).ratio() >= .85
            for index in range(max(0, len(words) - 1)))
        if text in actual or review_update:
            return
        if "Failed" in actual or process.poll() is not None:
            raise AssertionError(f"Installer failed while waiting for {text}: {actual}")
        time.sleep(2)
    raise AssertionError(f"Timed out waiting for {text}: {previous}")


def main():
    os.environ.setdefault("OMP_THREAD_LIMIT", "1")
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
        wait_page(probe, process, "Language")
        probe.frame("language")
        for page in (1, 3):
            next_page(probe, page)
        select_installation_mode(probe, update=True)
        next_page(probe, 4)
        probe.click(*probe.disk_selection_point())
        probe.click(*probe.footer_point())
        wait_page(probe, process, "Review update", timeout=600)
        assert probe.visible_page(probe.frame("inspection-complete")) == 4
        next_page(probe, 8)
        wait_page(probe, process, "Review system update", timeout=600)
        probe.frame("update-review")
        # Leave the inspected update unsubmitted, return to Mode, and carry
        # the same disk through fresh installation with independent defaults.
        previous_page(probe, 4)
        previous_page(probe, 3)
        select_installation_mode(probe, update=False)
        next_page(probe, 4)
        probe.click(*probe.disk_selection_point())
        next_page(probe, 5)
        next_page(probe, 6)
        next_page(probe, 11)
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
