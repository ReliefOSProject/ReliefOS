#!/usr/bin/env python3
"""Exercise the actual GUI wizard and coexistence install on owned scratch disks.

No host inspection reads a disk while QEMU owns it. The no-write case compares
the entire disk before/after reviewing a manual plan and rejecting confirmation.
The install case checks the original GPT and unrelated EFI/data contents after
normal guest shutdown, then boots that same installed disk into its real login.
"""
import argparse
from contextlib import contextmanager
import ctypes
import ctypes.util
import csv
from difflib import SequenceMatcher
import hashlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time
import uuid

from test_installer_accounts_qemu import (
    ROOT_PASSWORD, USER_PASSWORD, assert_desktop_profile, boot,
    debugfs_output, next_page, previous_page, select_installation_mode,
    wait_install, wait_log, xorg_checkbox_checked,
)

ROOT = Path(__file__).resolve().parents[1]
MIB = 1024**2
SECTOR = 512
OCR_SCALE = 2
BIOS_TYPE = "21686148-6449-6e6f-744e-656564454649"
ESP_TYPE = "c12a7328-f81f-11d2-ba4b-00a0c93ec93b"
ROOT_TYPE = "0fc63daf-8483-4772-8e79-3d69d8477de4"
DATA_TYPE = "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7"
EFI_PATH = "::/EFI/OTHEROS/PRESERVE.EFI"
EFI_SENTINEL = b"ReliefOS GUI coexistence: preserve another system's EFI file.\n"
DATA_SENTINEL = b"ReliefOS GUI coexistence: unrelated data must survive.\n"


def run(*command, **kwargs):
    return subprocess.run(list(map(str, command)), check=True, **kwargs)


def ocr(frame, psm=6, *, configs=()):
    with tempfile.NamedTemporaryFile(suffix=".png") as source:
        frame.resize((frame.width * OCR_SCALE, frame.height * OCR_SCALE)).save(source.name)
        return subprocess.run(["tesseract", source.name, "stdout", "--psm", str(psm), *configs],
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                              text=True, check=True, timeout=10,
                              env={**os.environ, "OMP_THREAD_LIMIT": "1"}).stdout


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


def partition_rows(table):
    """Mirror the public sector-sorted display, including >=1-MiB free gaps."""
    first, rows = 34, []
    minimum = (MIB + table["sectorsize"] - 1) // table["sectorsize"]
    for part in sorted(table["partitions"], key=lambda entry: entry["start"]):
        if part["start"] - first >= minimum:
            rows.append(None)
        rows.append(part["name"])
        first = part["start"] + part["size"]
    if table["lastlba"] + 1 - first >= minimum:
        rows.append(None)
    return rows


class ScratchDisk:
    """A newly created regular file; a running VM excludes all host reads."""
    def __init__(self, output):
        output.mkdir(parents=True, exist_ok=False)
        self.output = output.resolve()
        self.path = self.output / "scratch.raw"
        self.active = False
        with self.path.open("xb") as disk:
            disk.truncate(4096 * MIB)
        self.prepare()

    def inactive(self):
        if self.active:
            raise RuntimeError("Host disk inspection is forbidden while QEMU owns the disk")

    def table(self):
        self.inactive()
        return json.loads(subprocess.check_output([
            "sfdisk", "--json", self.path], text=True))["partitiontable"]

    def prepare(self):
        self.inactive()
        # Deliberate gaps before root/data ensure browser selection cannot
        # confuse a display row with the GPT partition's physical index.
        specification = (
            "label: gpt\nunit: sectors\n\n"
            f"start=2048,size=2048,type={BIOS_TYPE},name=FLOW_BIOS\n"
            f"start=4096,size=524288,type={ESP_TYPE},name=FLOW_ESP\n"
            f"start=561152,size=6291456,type={ROOT_TYPE},name=FLOW_ROOT\n"
            f"start=6854656,size=524288,type={DATA_TYPE},name=FLOW_DATA\n")
        run("sfdisk", self.path, input=specification, text=True,
            stdout=subprocess.DEVNULL)
        self.before = self.table()
        self.rows = partition_rows(self.before)
        write_json(self.output / "layout-before.json", self.before)
        esp = self.part("FLOW_ESP")
        fat = self.output / "esp-fixture.fat"
        with fat.open("xb") as image:
            image.truncate(esp["size"] * SECTOR)
        run("mkfs.fat", "-F", "32", "-n", "OTHEROS_EFI", fat,
            stdout=subprocess.DEVNULL)
        run("mmd", "-i", fat, "::/EFI", "::/EFI/OTHEROS")
        marker = self.output / "efi-sentinel"
        marker.write_bytes(EFI_SENTINEL)
        run("mcopy", "-i", fat, marker, EFI_PATH)
        # Copy only allocated FAT metadata and our tiny file, keeping the
        # scratch disk sparse even on filesystems without reflinks.
        run("dd", f"if={fat}", f"of={self.path}", "bs=1M",
            f"seek={esp['start'] * SECTOR // MIB}", "conv=notrunc,sparse",
            "status=none")
        fat.unlink()
        marker.unlink()
        data = self.part("FLOW_DATA")
        self.data_offsets = (0, 17 * MIB, data["size"] * SECTOR - 4096)
        with self.path.open("r+b") as disk:
            for offset in self.data_offsets:
                disk.seek(data["start"] * SECTOR + offset)
                disk.write(DATA_SENTINEL)
            disk.flush()
            os.fsync(disk.fileno())
        self.data_before = self.digest(data["start"] * SECTOR,
                                       data["size"] * SECTOR)

    def part(self, name):
        return next(part for part in self.before["partitions"] if part["name"] == name)

    def digest(self, offset=0, size=None):
        self.inactive()
        result = hashlib.sha256()
        with self.path.open("rb") as disk:
            disk.seek(offset)
            remaining = self.path.stat().st_size - offset if size is None else size
            while remaining:
                block = disk.read(min(4 * MIB, remaining))
                if not block:
                    raise AssertionError("Short disk read")
                result.update(block)
                remaining -= len(block)
        return result.hexdigest()

    @contextmanager
    def guest(self, name, iso=None, **kwargs):
        self.inactive()
        self.active = True
        try:
            with boot(self.output / name, self.path, iso, **kwargs) as session:
                yield session
        finally:
            # boot() quits QEMU and waits for its process before returning.
            self.active = False

    def verify_preserved(self):
        self.inactive()
        after = self.table()
        write_json(self.output / "layout-after.json", after)
        assert after == self.before, "Manual installation changed the original GPT"
        esp, data = self.part("FLOW_ESP"), self.part("FLOW_DATA")
        source = f"{self.path}@@{esp['start'] * SECTOR}"
        content = subprocess.check_output(["mtype", "-i", source, EFI_PATH])
        assert content == EFI_SENTINEL, "Another system's EFI file changed"
        assert self.digest(data["start"] * SECTOR, data["size"] * SECTOR) == self.data_before, \
            "An unrelated data partition changed"
        with self.path.open("rb") as disk:
            for offset in self.data_offsets:
                disk.seek(data["start"] * SECTOR + offset)
                assert disk.read(len(DATA_SENTINEL)) == DATA_SENTINEL
        return source


def content_ocr(probe, name, box, *, psm=6):
    x1, y1, x2, y2 = box
    assert probe.body_offset == 0, "Installer page origin changed"
    assert 0 <= x1 < x2 <= probe.content_width and \
        0 <= y1 < y2 <= probe.body_viewport_height(), \
        f"OCR region {box} is outside the visible page"
    frame = probe.frame(name)
    left, top = probe.content_left, probe.sidebar_top
    crop = frame.crop((left + x1, top + y1, left + x2, top + y2))
    crop.save(probe.output / f"{name}-crop.png")
    # White text on the native blue browser selection can disappear during
    # Tesseract's page thresholding. Normalize that actual row's contrast.
    normalized = normalize_selection(crop)
    normalized.save(probe.output / f"{name}-ocr.png")
    text = ocr(normalized, psm=psm)
    (probe.output / f"{name}.txt").write_text(text)
    return text


def contains(text, *needles):
    actual = re.sub(r"[^a-z0-9]+", "", text.lower())
    for needle in needles:
        if needle.startswith("[") and needle.endswith("]"):
            assert re.search(r"\[\s*" + re.escape(needle[1:-1]) + r"\s*\]", text, re.I), \
                f"Missing explicit role marker {needle!r} in GUI text: {text!r}"
            continue
        expected = re.sub(r"[^a-z0-9]+", "", needle.lower())
        assert expected in actual, f"Missing {needle!r} in GUI text: {text!r}"


def wait_content(probe, name, box, *needles, validator=None, timeout=12, psm=6):
    """Require the actual rendered state, allowing bounded asynchronous redraw."""
    deadline = time.monotonic() + timeout
    while True:
        text = content_ocr(probe, name, box, psm=psm)
        try:
            contains(text, *needles)
            if validator is not None:
                validator(text)
            return text
        except AssertionError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(.25)


def require_partition_role(text, partition, role):
    lines = [line for line in text.splitlines()
             if re.sub(r"[^a-z0-9]+", "", partition.lower()) in
             re.sub(r"[^a-z0-9]+", "", line.lower())]
    assert len(lines) == 1, f"Could not identify partition row: {text!r}"
    contains(lines[0], {"esp": "[ESP]", "root": "[root]", "bios": "BIOS Boot"}[role])


def visible_text_point(probe, name, box, needle):
    """Locate actual text in the current frame, including browser free rows."""
    from PIL import Image
    content_ocr(probe, name, box)
    with Image.open(probe.output / f"{name}-ocr.png") as source:
        text = ocr(source, configs=("tsv",))
    groups = {}
    for entry in csv.DictReader(io.StringIO(text), delimiter="\t"):
        if entry["level"] != "5" or not entry.get("text", "").strip():
            continue
        key = tuple(entry[field] for field in ("block_num", "par_num", "line_num"))
        groups.setdefault(key, []).append(entry)
    expected = re.sub(r"[^a-z0-9]+", "", needle.lower())
    for entries in groups.values():
        actual = re.sub(r"[^a-z0-9]+", "", " ".join(e["text"] for e in entries).lower())
        if expected in actual:
            y1 = min(int(entry["top"]) for entry in entries)
            y2 = max(int(entry["top"]) + int(entry["height"]) for entry in entries)
            return probe.body_point(box[0] + 90, box[1] + (y1 + y2) // (2 * OCR_SCALE))
    raise AssertionError(f"Could not locate visible text {needle!r}: {text}")


def selected_partition_name(probe, partition):
    from test_installer_responsiveness import SIDEBAR_BLUE
    frame = probe.frame("partition-selection-check")
    _, browser_top, _, browser_bottom = probe.partition_browser_box()
    x1, y1 = probe.body_point(2, browser_top + 2)
    x2, y2 = probe.body_point(probe.content_width - 2, browser_bottom - 2)
    browser = frame.crop((x1, y1, x2, y2))
    rows = [y for y in range(browser.height)
            if sum(browser.getpixel((x, y)) == SIDEBAR_BLUE for x in range(browser.width)) > browser.width * .5]
    assert rows, "The partition browser has no visibly selected row"
    image = normalize_selection(browser.crop((0, min(rows), browser.width, max(rows) + 1)))
    image.save(probe.output / f"selected-row-{partition}.png")
    text = ocr(image, psm=7)
    contains(text, partition)
    return text


def normalize_selection(crop):
    from test_installer_responsiveness import SIDEBAR_BLUE
    result = crop.convert("RGB")
    pixels = result.load()
    selected_rows = []
    for y in range(result.height):
        if sum(pixels[x, y] == SIDEBAR_BLUE for x in range(result.width)) > result.width * .4:
            selected_rows.append(y)
            for x in range(result.width):
                # Undo the white glyph's alpha blend over blue. Keeping its
                # antialiasing matters at 13px; a hard white threshold erases
                # most strokes. Dark focus-frame pixels become background.
                red, green, blue = pixels[x, y]
                level = 255 - max(0, min(255, (red - SIDEBAR_BLUE[0]) * 255 // (255 - SIDEBAR_BLUE[0]))) \
                    if green >= SIDEBAR_BLUE[1] and blue >= SIDEBAR_BLUE[2] else 255
                pixels[x, y] = (level, level, level)
    # The focused row has dark dotted rules immediately outside its blue
    # interior. Their full-width patterns can hide both adjacent OCR lines.
    for y in selected_rows:
        for border in (y - 1, y + 1):
            if 0 <= border < result.height and border not in selected_rows:
                for x in range(result.width):
                    pixels[x, border] = (255, 255, 255)
    return result


def point(probe, x, y, content_height=None):
    return probe.body_point(x, y, content_height=content_height)


def choose_desktop(probe, index):
    probe.click(*probe.desktop_option_point())
    for _ in range(2):
        probe.key("up")
    for _ in range(index):
        probe.key("down")
    probe.key("ret")
    probe.move(probe.width - 10, 10)
    wait_content(probe, f"desktop-{index}", (5, 160, probe.content_width - 20, 181),
                 ("default", "experimental", "only")[index])


def set_field(probe, location, value):
    probe.click(*location)
    probe.key("home")
    probe.key("shift-end")
    probe.text(value)


def assert_unchecked(probe, location, name):
    probe.move(probe.width - 10, 10)
    frame = probe.frame(name)
    x, y = location
    assert not xorg_checkbox_checked(frame.crop((x - 9, y - 9, x + 10, y + 10))), \
        f"{name} unexpectedly enabled"


def select_role(probe, scratch, partition, role):
    assert role in ("esp", "root", "bios")
    assert partition in scratch.rows
    browser = probe.partition_browser_box()
    probe.click(*visible_text_point(probe, f"locate-{partition}",
                                    browser, partition))
    deadline = time.monotonic() + 8
    while True:
        try:
            selected_partition_name(probe, partition)
            break
        except AssertionError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(.25)
    wait_content(probe, f"selected-{partition}", browser, partition)
    # Use the native role menu and its separate Apply button. Selecting a
    # browser row alone must never change the partition role.
    choice_x = probe.content_width // 4 + 12
    choice_width = probe.content_width - probe.content_width // 4 - 112
    role_y = probe.body_viewport_height() - 168
    probe.click(*point(probe, choice_x + choice_width // 2, role_y + 15))
    for _ in range(4):
        probe.key("up")
    for _ in range({"esp": 0, "root": 1, "bios": 3}[role]):
        probe.key("down")
    probe.key("ret")
    probe.click(*point(probe, probe.content_width - 44, role_y + 15))
    # Require the role on that partition's own line. A root/ESP marker on a
    # different row must not turn a mistaken free-space selection into a pass.
    wait_content(probe, f"role-{partition}-{role}", browser, partition,
                 validator=lambda text: require_partition_role(text, partition, role))


def wait_gui_startup(probe, serial, process, timeout):
    """Observe the real framebuffer while booting, including GRUB failures."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("QEMU exited before the installer appeared")
        frame = probe.frame("startup-progress")
        if probe.visible_page(frame) == 0:
            from test_installer_responsiveness import CONTENT_BACKGROUND
            x1, y1, _, _ = probe.footer_button_box()
            # TCG may expose the sidebar before the body/footer draw finishes.
            # Wait for the actual primary button before sending real input.
            if frame.getpixel((x1 + 6, y1 + 6)) != CONTENT_BACKGROUND:
                frame.save(probe.output / "startup-language-ready.png")
                return frame
        log = serial.read_text(errors="replace") if serial.exists() else ""
        text = ""
        # Terminal boot logs contain thousands of glyphs and already have a
        # serial record. Limit OCR to firmware/GRUB screens and keep each
        # observation bounded, without consuming the VM's CPU with OpenMP.
        if "[reliefnt] boot complete:" not in log:
            source = probe.output / "startup-ocr-input.png"
            sample = frame.copy()
            sample.thumbnail((1280, 800))
            sample.save(source)
            try:
                text = subprocess.run(["tesseract", str(source), "stdout", "--psm", "6"],
                                      stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                      text=True, check=True, timeout=min(10, max(.1, deadline - time.monotonic())),
                                      env={**os.environ, "OMP_THREAD_LIMIT": "1"}).stdout
            except subprocess.TimeoutExpired:
                text = "OCR observation exceeded its deadline; see startup-progress.png"
            (probe.output / "startup-ocr.txt").write_text(text)
        normalized = re.sub(r"[^a-z0-9]+", "", text.lower())
        if any(marker in normalized for marker in ("outofmemory", "unalignedpointer", "kernelpanic")) or "KERNEL PANIC" in log:
            frame.save(probe.output / "startup-error.png")
            raise AssertionError("Guest boot failed before the installer: " + text.strip())
        time.sleep(2)
    raise AssertionError(f"Installer language page did not appear within {timeout}s; see startup-progress.png")


def resize_installer(probe, width, height):
    """Resize the real managed window by dragging its bottom-right border."""
    assert probe.visible_page(probe.frame("before-resize")) == 0
    left, top = probe.sidebar_left, probe.sidebar_top
    start_x, start_y = probe.window_right + 1, probe.sidebar_bottom + 1
    end_x, end_y = left + width + 1, top + height + 1
    probe.move(start_x, start_y)
    probe.button(True)
    time.sleep(.15)
    for step in range(1, 17):
        probe.move(round(start_x + (end_x - start_x) * step / 16),
                   round(start_y + (end_y - start_y) * step / 16))
        time.sleep(.04)
    probe.button(False)
    time.sleep(1)
    probe.clear_installer_geometry()
    frame = probe.frame(f"resized-{width}x{height}")
    assert probe.visible_page(frame) == 0, "Installer language sidebar disappeared after resize"
    actual = (probe.window_right - probe.sidebar_left, probe.sidebar_bottom - probe.sidebar_top)
    assert actual == (width, height), f"Window resize produced {actual}, expected {(width, height)}"
    for previous in (True, False):
        probe.footer_button_box(previous)
    write_json(probe.output / "resize-evidence.json", {
        "width": width, "height": height, "content_width": probe.content_width,
        "body_viewport_height": probe.body_viewport_height(), "actual_mouse_drag": True,
    })


def review_manual(probe, serial, process, scratch, timeout, *, small_window=False):
    frame = wait_gui_startup(probe, serial, process, timeout)
    assert probe.visible_page(frame) == 0
    if small_window:
        resize_installer(probe, 768, 540)
    probe.click(*probe.language_option_point())
    next_page(probe, 1)
    # The license has a real gate; clicking Continue without acceptance stays.
    probe.click(*probe.footer_point())
    assert probe.visible_page(probe.frame("license-unaccepted")) == 1
    next_page(probe, 3)
    select_installation_mode(probe, update=False)
    next_page(probe, 4)
    probe.click(*probe.footer_point())
    assert probe.visible_page(probe.frame("disk-not-selected")) == 4
    probe.click(*probe.disk_selection_point())
    next_page(probe, 5)
    for bios in (True, False):
        x, y = probe.bootloader_indicator_point(bios=bios)
        frame = probe.frame("bootloaders-default")
        assert xorg_checkbox_checked(frame.crop((x - 9, y - 9, x + 10, y + 10)))
    # Modify and revisit firmware selection to verify retained state.
    probe.click(*probe.bootloader_indicator_point(bios=True))
    previous_page(probe, 4)
    next_page(probe, 5)
    assert_unchecked(probe, probe.bootloader_indicator_point(bios=True), "bios-retained-disabled")
    probe.click(*probe.bootloader_indicator_point(bios=True))
    next_page(probe, 6)
    probe.click(*probe.partition_method_point())
    time.sleep(.5)
    browser = probe.partition_browser_box()
    wait_content(probe, "manual-layout", (0, 82, probe.content_width, browser[3]), "Manual", "Unallocated")
    probe.scroll_body(bottom=True)
    from test_installer_responsiveness import SIDEBAR_BLUE
    expected_inside = point(probe, probe.content_width - 8, browser[1] + 4)
    deadline = time.monotonic() + 8
    while True:
        frame = probe.frame("manual-layout-geometry")
        if frame.getpixel(expected_inside) in ((255, 255, 255), SIDEBAR_BLUE):
            break
        assert time.monotonic() < deadline, \
            "Partition browser moved from its planned geometry; the content group may have stretched child widgets"
        time.sleep(.25)
    select_role(probe, scratch, "FLOW_ROOT", "root")
    select_role(probe, scratch, "FLOW_ESP", "esp")
    select_role(probe, scratch, "FLOW_BIOS", "bios")
    assert_unchecked(probe, probe.partition_format_indicator_point(), "esp-format-unchecked")
    next_page(probe, 11)
    choose_desktop(probe, 2)
    previous_page(probe, 6)
    wait_content(probe, "manual-roles-retained", probe.partition_browser_box(), "[root]", "[ESP]")
    next_page(probe, 11)
    wait_content(probe, "desktop-retained", (5, 160, probe.content_width - 20, 181), "only")
    choose_desktop(probe, 0)
    assert_unchecked(probe, probe.swap_indicator_point(), "swap-default-disabled")
    next_page(probe, 7)
    for index, value in enumerate(("alice", USER_PASSWORD, USER_PASSWORD, ROOT_PASSWORD, ROOT_PASSWORD)):
        set_field(probe, probe.account_field_point(index), value)
    next_page(probe, 8)
    wait_content(probe, "review-before-back", (0, 96, probe.content_width, 220), "alice", "IceWM", "BIOS", "UEFI")
    previous_page(probe, 7)
    wait_content(probe, "account-name-retained", (5, 108, probe.content_width - 5, 129), "alice")
    # Successful revalidation proves both password confirmations survived Back.
    next_page(probe, 8)
    wait_content(probe, "manual-plan-summary", (0, 284, probe.content_width, probe.body_viewport_height() - 100),
                 "System root", "ESP", "preserve")
    probe.scroll_body()
    set_field(probe, probe.confirm_field_point(), "NO")
    probe.click(*probe.footer_point())
    time.sleep(1)
    assert probe.visible_page(probe.frame("invalid-confirmation")) == 8
    assert "installation completed successfully" not in serial.read_text(errors="replace")


def poweroff(probe, process, timeout=90):
    probe.key("ctrl-alt-f2")
    time.sleep(1)
    probe.text("/sbin/poweroff")
    probe.key("ret")
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            assert process.returncode == 0, "QEMU failed during shutdown"
            return
        if probe.command("query-status")["status"] == "shutdown":
            return
        time.sleep(.2)
    raise AssertionError("The guest did not finish normal shutdown")


def password_matches(password, encoded):
    library = ctypes.CDLL(ctypes.util.find_library("crypt") or "libcrypt.so.1")
    library.crypt.argtypes = (ctypes.c_char_p, ctypes.c_char_p)
    library.crypt.restype = ctypes.c_char_p
    result = library.crypt(password.encode(), encoded.encode())
    return result is not None and result.decode() == encoded


def inspect_install(scratch):
    source = scratch.verify_preserved()
    root = scratch.part("FLOW_ROOT")
    instance = f"::/reliefos/instances/{root['uuid'].lower()}"
    bootloaders = subprocess.check_output([
        "mtype", "-i", source, instance + "/reliefos/config/bootloaders.conf"], text=True)
    assert bootloaders == "version=1\nbios=1\nuefi=1\n", bootloaders
    run("mdir", "-i", source, "::/EFI/BOOT/BOOTX64.EFI", stdout=subprocess.DEVNULL)
    image = scratch.output / "installed-root.ext4"
    run("dd", f"if={scratch.path}", f"of={image}", "bs=1M", "iflag=skip_bytes,count_bytes",
        f"skip={root['start'] * SECTOR}", f"count={root['size'] * SECTOR}", "conv=sparse", "status=none")
    try:
        passwd = {line.split(":", 1)[0]: line.split(":")
                  for line in debugfs_output(image, "cat /etc/passwd").splitlines()}
        shadow = {line.split(":", 1)[0]: line.split(":")[1]
                  for line in debugfs_output(image, "cat /etc/shadow").splitlines()}
        assert passwd["alice"][2:4] == ["1000", "1000"]
        assert passwd["alice"][5] == "/home/alice"
        assert passwd["root"][2:4] == ["0", "0"]
        assert password_matches(USER_PASSWORD, shadow["alice"]), "Ordinary password was not retained"
        assert password_matches(ROOT_PASSWORD, shadow["root"]), "Root password was not retained"
        assert_desktop_profile(image, False)
        fstab = debugfs_output(image, "cat /etc/fstab")
        assert root["uuid"].lower() in fstab.lower(), fstab
        report = {"gpt_preserved": True, "efi_sentinel_preserved": True,
                  "data_sha256": scratch.data_before, "accounts_verified": ["root", "alice"],
                  "desktop": "xorg", "bios": True, "uefi": True}
        write_json(scratch.output / "installed-evidence.json", report)
        return report
    finally:
        image.unlink(missing_ok=True)


def wait_terminal_prompt(probe, process, name, needle, timeout=30):
    """Read the current terminal line without matching old login scrollback."""
    deadline = time.monotonic() + timeout
    visible = ""
    while time.monotonic() < deadline:
        assert process.poll() is None, "QEMU exited before terminal prompt"
        frame = probe.frame(name)
        bounds = frame.getbbox()
        if bounds:
            bottom = bounds[3]
            crop = frame.crop((0, max(0, bottom - 64), min(frame.width, 1100), bottom))
            crop.save(probe.output / (name + "-tail.png"))
            visible = ocr(crop)
            lines = [line.strip() for line in visible.splitlines()
                     if re.search(r"[a-z0-9]", line, re.I)]
            expected = re.sub(r"[^a-z0-9]+", "", needle.lower())
            current = re.sub(r"[^a-z0-9]+", "", lines[-1].lower()) if lines else ""
            password_label = (expected == "password" and
                              SequenceMatcher(None, expected, current).ratio() >= .85)
            if expected and (expected in current or password_label):
                return visible
        time.sleep(.2)
    raise AssertionError(f"Missing current terminal prompt {needle!r}: {visible!r}")


def verify_boot(scratch, options, timeout):
    from test_dual_boot_qemu import wait_xdm
    with scratch.guest("installed-boot-" + options["firmware"], **options) as (probe, serial, process):
        wait_log(serial, "[reliefnt] PID 1 path=", process, timeout=timeout)
        expected = "UEFI" if options["firmware"] == "uefi" else "legacy"
        wait_log(serial, "[boot] firmware=" + expected, process, timeout=timeout)
        wait_xdm(probe, process, "installed-xdm", timeout)
        # Console login expires after 60 guest seconds. Enter keys promptly
        # and OCR only the current prompt before submitting the password.
        def console_key(key):
            probe.command("human-monitor-command", {"command-line": f"sendkey {key} 5"})
            time.sleep(.06)
        probe.key = console_key
        probe.key("ctrl-alt-f2")
        wait_terminal_prompt(probe, process, "installed-root-login", "login:", timeout=timeout)
        probe.text("root")
        probe.key("ret")
        wait_terminal_prompt(probe, process, "installed-root-password", "Password:", timeout=timeout)
        probe.text(ROOT_PASSWORD)
        probe.key("ret")
        token = "FLOW_ROOT_LOGIN_OK_" + uuid.uuid4().hex
        probe.text("test $(id -u) = 0 && "
                   "echo INSTALLER_USER_PRESERVE >/home/alice/installer-user-check && "
                   "chown alice:alice /home/alice/installer-user-check && "
                   "echo INSTALLER_ROOT_PRESERVE >/root/installer-root-check && sync && "
                   "echo " + token + " >/dev/ttyS0")
        probe.key("ret")
        wait_log(serial, token, process, timeout=timeout)
        probe.frame("installed-root-authenticated")
        poweroff(probe, process)
    scratch.verify_preserved()


def execute(args):
    args.output.mkdir(parents=True, exist_ok=False)
    options = {"accel": args.accel, "firmware": args.firmware,
               "firmware_image": args.firmware_image}
    results = {}
    if args.case in ("all", "no-write"):
        scratch = ScratchDisk(args.output / "no-write")
        before = scratch.digest()
        write_json(scratch.output / "no-write-before.json", {"sha256": before})
        with scratch.guest("review", args.iso.resolve(), **options) as session:
            review_manual(*session, scratch, args.timeout, small_window=args.small_window)
            # Return from Summary through every configuration page. The shared
            # model retains the actual choices; no installation is submitted.
            for page in (7, 11, 6, 5, 4, 3, 1, 0):
                previous_page(session[0], page)
            session[0].frame("review-returned-language")
        after = scratch.digest()
        assert before == after, "The test disk changed before INSTALL confirmation"
        scratch.verify_preserved()
        results["no-write"] = {"before_sha256": before, "after_sha256": after}
        write_json(scratch.output / "no-write-evidence.json", results["no-write"])
        print("PASS GUI review, retained choices, invalid confirmation and whole-disk no-write", flush=True)
    if args.case in ("all", "install"):
        scratch = ScratchDisk(args.output / "install")
        with scratch.guest("install", args.iso.resolve(), **options) as session:
            probe, serial, process = session
            review_manual(*session, scratch, args.timeout)
            set_field(probe, probe.confirm_field_point(), "INSTALL")
            next_page(probe, 9)
            wait_install(probe, serial, process, timeout=args.install_timeout)
            poweroff(probe, process)
        results["install"] = inspect_install(scratch)
        firmwares = ("bios", "uefi") if args.boot_firmware == "both" else (args.boot_firmware or args.firmware,)
        for firmware in firmwares:
            verify_boot(scratch, {**options, "firmware": firmware}, args.timeout)
        results["install"]["boot_firmwares"] = list(firmwares)
        results["install"]["xdm_and_root_login"] = True
        print("PASS manual coexistence: original GPT, unrelated data/EFI, accounts, desktop and installed boot", flush=True)
    write_json(args.output / "results.json", results)


def self_test():
    """Check real GPT/FAT fixtures and guards without building or starting QEMU."""
    from PIL import Image, ImageDraw
    from test_installer_responsiveness import (
        CONTENT_BACKGROUND, InstallerProbe, SIDEBAR_ACTIVE, SIDEBAR_BLUE,
        SIDEBAR_PAGES, UPDATE_SIDEBAR_PAGES,
    )
    # Partition names themselves must never satisfy an assigned-role check.
    for marker in ("root", "ESP"):
        contains(f"Partition FLOW_{marker} ... [{marker}]", f"[{marker}]")
        try:
            contains(f"Partition FLOW_{marker} ...", f"[{marker}]")
        except AssertionError:
            pass
        else:
            raise AssertionError("A partition name bypassed explicit role verification")
    selected = Image.new("RGB", (200, 14), SIDEBAR_BLUE)
    ImageDraw.Draw(selected).rectangle((8, 4, 16, 9), fill="white")
    normalized = normalize_selection(selected)
    assert normalized.getpixel((1, 5)) == (255, 255, 255)
    assert normalized.getpixel((10, 5)) == (0, 0, 0)
    # Include the minimum client inside a real 800x600 framebuffer, alongside
    # larger desktop cases. Long labels must not break current-row detection.
    for update, pages in ((False, SIDEBAR_PAGES), (True, UPDATE_SIDEBAR_PAGES)):
        for width, height, screen_width, screen_height in (
                (768, 540, 800, 600), (800, 540, 1920, 1080), (1024, 700, 1920, 1080)):
            for selected, expected in enumerate(pages):
                frame = Image.new("RGB", (screen_width, screen_height), "black")
                draw = ImageDraw.Draw(frame)
                draw.rectangle((6, 26, 6 + width - 1, 26 + height - 1), fill=CONTENT_BACKGROUND)
                draw.rectangle((6, 26, 193, 26 + height - 1), fill=SIDEBAR_BLUE)
                for row in range(len(pages)):
                    y = 126 + row * 34
                    if row == selected:
                        draw.rectangle((18, y, 181, y + 29), fill=SIDEBAR_ACTIVE)
                    # The label reaches the right sampling columns. Its glyphs
                    # must not hide the active row's background color.
                    draw.rectangle((158, y + 8, 181, y + 22), fill=(34, 36, 46))
                probe = InstallerProbe.__new__(InstallerProbe)
                probe._reset_installer_geometry()
                probe.width, probe.height = frame.size
                probe.installer_update_mode = update
                assert probe.visible_page(frame) == expected, (update, selected, expected)
                assert probe.content_width == width - 212 - 24
                assert probe.window_right == 6 + width
                assert probe.body_viewport_height() == height - 80
                assert probe.footer_point() == (6 + 366 + 74, 26 + height - 56 + 19)
                assert probe.footer_point(previous=True) == (6 + 212 + 68, 26 + height - 56 + 19)
                assert probe.account_field_point(0) == (6 + 212 + probe.content_width // 2, 26 + 117)
                assert probe.desktop_option_point()[1] == 26 + 169
                assert probe.license_indicator_point() == (6 + 212 + 8, 26 + height - 80 - 31)
                assert probe.confirm_field_point() == (6 + 212 + probe.content_width // 4,
                                                       26 + height - 80 - 32)
                assert probe.body_point(0, 0) == (probe.content_left, probe.sidebar_top)
                for invalid in ((-1, 0), (probe.content_width, 0), (0, height - 80)):
                    try:
                        probe.body_point(*invalid)
                    except AssertionError:
                        pass
                    else:
                        raise AssertionError(f"An off-page input coordinate was accepted: {invalid}")
                # Match stationary foreground. Moving the page must fail even
                # though most pixels are unchanged background.
                body = Image.new("RGB", (probe.content_width, 900), CONTENT_BACKGROUND)
                painter = ImageDraw.Draw(body)
                for y in range(0, 900, 31):
                    painter.rectangle((8, y, body.width - 9, y + 7), fill=(40 + y % 120, 80, 130))
                before, after = frame.copy(), frame.copy()
                viewport = probe.body_viewport_height()
                before.paste(body.crop((0, 0, body.width, viewport)), (probe.content_left, 26))
                after.paste(body.crop((0, 100, body.width, 100 + viewport)), (probe.content_left, 26))
                probe._assert_body_stationary(before, before)
                try:
                    probe._assert_body_stationary(before, after)
                except AssertionError:
                    pass
                else:
                    raise AssertionError("Page movement bypassed the fixed-page check")
                # A native child may receive a gutter wheel event through
                # Fl_Group fallback. Its text/list scrolling is legal, while
                # a moved acceptance/confirmation field or toolbar is not.
                if expected in (1, 4, 6, 8) and not (expected == 8 and update):
                    for manual in ((True, False) if expected == 6 else (True,)):
                        nested_top, nested_bottom = {
                            1: (104, viewport - 64),
                            4: (154, viewport - (216 if update else 94)),
                            6: (164, viewport - (212 if manual else 100)),
                            8: (284, viewport - 100),
                        }[expected]
                        fixed = frame.copy()
                        painter = ImageDraw.Draw(fixed)
                        left, top = probe.content_left, probe.sidebar_top
                        painter.text((left + 8, top + 28), "Fixed page heading", fill="black")
                        nested_box = (left, top + nested_top,
                                      left + probe.content_width, top + nested_bottom)
                        painter.rectangle((nested_box[0], nested_box[1],
                                           nested_box[2] - 1, nested_box[3] - 1), fill="white")
                        for row in range(nested_top + 4, nested_bottom - 8, 20):
                            painter.text((left + 12, top + row), "Scrollable document or list row",
                                         fill="black")
                        control_y = (viewport - 48 if expected in (1, 8) else
                                     viewport - (180 if update else 80) if expected == 4 else
                                     viewport - (204 if manual else 56))
                        control_box = (left, top + control_y, left + 260, top + control_y + 30)
                        painter.rectangle((control_box[0], control_box[1],
                                           control_box[2] - 1, control_box[3] - 1),
                                          fill="white", outline="black", width=2)
                        painter.text((left + 8, top + control_y + 8),
                                     "Accept terms / INSTALL / fixed settings", fill="black")
                        scrolled = fixed.copy()
                        scrolled.paste(Image.new("RGB", (probe.content_width,
                                                        nested_bottom - nested_top), "white"),
                                       (left, top + nested_top))
                        painter = ImageDraw.Draw(scrolled)
                        for row in range(nested_top + 4, nested_bottom - 8, 20):
                            painter.text((left + 12, top + row), "Different internally scrolled row",
                                         fill="black")
                        assert probe._body_fixed_bands(fixed) == [
                            (0, nested_top), (nested_bottom, viewport)]
                        probe._assert_body_stationary(fixed, scrolled)
                        moved_control = scrolled.copy()
                        painter = ImageDraw.Draw(moved_control)
                        painter.rectangle((control_box[0], control_box[1],
                                           control_box[2] - 1, control_box[3] - 1),
                                          fill=CONTENT_BACKGROUND)
                        moved_control.paste(fixed.crop(control_box),
                                            (control_box[0], control_box[1] + 4))
                        try:
                            probe._assert_body_stationary(fixed, moved_control)
                        except AssertionError:
                            pass
                        else:
                            raise AssertionError(
                                f"Page {expected} moved fixed control bypassed nested-scroll exclusion")
    with tempfile.TemporaryDirectory(prefix="installer-gui-flow-") as directory:
        from test_installer_window_qemu import firmware_arguments
        code, variables = Path(directory) / "OVMF_CODE_4M.fd", Path(directory) / "OVMF_VARS_4M.fd"
        code.write_bytes(b"C" * 6)
        variables.write_bytes(b"V" * 2)
        arguments = firmware_arguments(image=code)
        assert "readonly=on" in arguments[1] and "snapshot=on" in arguments[3]
        assert variables.read_bytes() == b"VV"
        complete = Path(directory) / "OVMF.fd"
        complete.write_bytes(b"F" * 8)
        assert firmware_arguments(image=complete) == ["-bios", str(complete)]
        scratch = ScratchDisk(Path(directory) / "fixture")
        assert scratch.rows == ["FLOW_BIOS", "FLOW_ESP", None, "FLOW_ROOT", None, "FLOW_DATA", None]
        assert scratch.part("FLOW_ROOT")["size"] * SECTOR == 3072 * MIB
        assert scratch.path.stat().st_blocks * SECTOR < 8 * MIB, "Fixture unexpectedly stopped being sparse"
        scratch.verify_preserved()
        before = scratch.digest()
        scratch.active = True
        for action in (scratch.table, scratch.digest, scratch.verify_preserved):
            try:
                action()
            except RuntimeError:
                pass
            else:
                raise AssertionError("A host read bypassed the active-VM guard")
        scratch.active = False
        assert scratch.digest() == before
        reference = "$6$flow$vmPKtYT55YlwrBQ12/7yxhA9buDmGArPXYFhmEIcDz1qXIpEttrFLLleuPHJn6koHJcOGxIAiZXv1F5zfrdEN1"
        assert password_matches("password", reference)
        assert not password_matches("wrong-password", reference)
        try:
            ScratchDisk(scratch.output)
        except FileExistsError:
            pass
        else:
            raise AssertionError("An existing output fixture was accepted")
    print("PASS self-test: 768/800/1024 GUI geometry, fixed pages and nested scrolling, "
          "moved controls rejected, sparse GPT/FAT fixture and active-VM read guards")


def main():
    # OCR shares the host with a full VM; OpenMP oversubscription can make a
    # dense terminal capture take minutes and obscure the actual boot state.
    os.environ.setdefault("OMP_THREAD_LIMIT", "1")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", type=Path, default=ROOT / "out/x86_64/release/images/reliefos-installer.iso")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--case", choices=("all", "no-write", "install"), default="all")
    parser.add_argument("--accel", choices=("auto", "kvm", "tcg"), default="auto")
    parser.add_argument("--firmware", choices=("bios", "uefi"), default="uefi")
    parser.add_argument("--boot-firmware", choices=("bios", "uefi", "both"))
    parser.add_argument("--firmware-image", type=Path)
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("--install-timeout", type=float, default=2400)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--small-window", action="store_true",
                        help="Resize the no-write wizard to the minimum 768x540 client through a real mouse drag")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if not args.output or not args.iso.is_file():
        parser.error("an existing --iso and a new --output directory are required")
    if args.timeout <= 0 or args.install_timeout <= 0:
        parser.error("timeouts must be positive")
    args.output = args.output.resolve()
    execute(args)


if __name__ == "__main__":
    main()
