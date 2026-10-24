"""Check a running installer's redraw latency through an isolated QEMU QMP socket.

Start the installer at its language page with no writable disk attached. This
test visits only Language, Thanks, Welcome, and Mode; it never starts a copy.
Requires Pillow. Screenshots and measured response times are saved in --output.
"""

import argparse
import json
from pathlib import Path
import socket
import time

from PIL import Image


SIDEBAR_BLUE = (0x3B, 0x62, 0xA6)
SIDEBAR_ACTIVE = (0xFF, 0xFF, 0xFF)
SIDEBAR_ROW_TOP = 100
SIDEBAR_ROW_PITCH = 34
SIDEBAR_ROW_HEIGHT = 30
SIDEBAR_PAGE_COUNT = 10


class InstallerProbe:
    def __init__(self, socket_path, output):
        self.output = output.resolve()
        self.output.mkdir(parents=True, exist_ok=True)
        self._reset_installer_geometry()
        self.socket = socket.socket(socket.AF_UNIX)
        self.socket.settimeout(5)
        self.socket.connect(socket_path)
        self.stream = self.socket.makefile("rwb", buffering=0)
        self.stream.readline()
        self.command("qmp_capabilities")
        frame = self.screenshot("initial")
        self.width, self.height = frame.size
        assert self.visible_page(frame) == 0, "Start at the installer language page"

    def command(self, name, arguments=None):
        request = {"execute": name}
        if arguments is not None:
            request["arguments"] = arguments
        self.stream.write(json.dumps(request).encode() + b"\n")
        while True:
            reply = json.loads(self.stream.readline())
            if "error" in reply:
                raise RuntimeError(reply)
            if "return" in reply:
                return reply["return"]

    def move(self, x, y):
        self.command("input-send-event", {"events": [
            {"type": "abs", "data": {
                "axis": "x", "value": round(x * 32767 / (self.width - 1))}},
            {"type": "abs", "data": {
                "axis": "y", "value": round(y * 32767 / (self.height - 1))}},
        ]})

    def button(self, down):
        self.command("input-send-event", {"events": [
            {"type": "btn", "data": {"button": "left", "down": down}},
        ]})

    def screenshot(self, name):
        path = self.output / "frame.ppm"
        self.command("screendump", {"filename": str(path)})
        with Image.open(path) as source:
            frame = source.convert("RGB")
        frame.save(self.output / f"{name}.png")
        return frame

    def _reset_installer_geometry(self):
        self._installer_sidebar = None
        self.sidebar_left = None
        self.sidebar_top = None
        self.sidebar_bottom = None
        self.sidebar_right = None
        self.content_left = None

    def _detect_installer_sidebar(self, frame):
        cached = self._installer_sidebar
        if cached and cached[:2] == frame.size:
            _, _, left, top, right, bottom = cached
            if (0 <= left + 4 < frame.width and 0 <= top + 4 < frame.height and
                    frame.getpixel((left + 4, top + 4)) == SIDEBAR_BLUE):
                return left, top, right, bottom

        pixels = frame.load()
        blue_columns = []
        # The active white row and sidebar labels interrupt exact-color blue
        # runs inside otherwise full-height columns. A 45% column threshold
        # still distinguishes this tall pane from incidental blue controls.
        column_threshold = int(frame.height * 0.45)
        for x in range(frame.width):
            count = 0
            for y in range(frame.height):
                if pixels[x, y] == SIDEBAR_BLUE:
                    count += 1
            if count >= column_threshold:
                blue_columns.append((x, count))

        runs = []
        for x, count in blue_columns:
            if not runs or x != runs[-1][-1][0] + 1:
                runs.append([(x, count)])
            else:
                runs[-1].append((x, count))
        candidates = [run for run in runs if len(run) >= 80]
        if not candidates:
            return None
        run = max(candidates, key=lambda columns: (len(columns),
                                                    sum(count for _, count in columns)))
        left, right = run[0][0], run[-1][0] + 1
        sample_x = min(right - 1, left + 4)
        blue_rows = [y for y in range(frame.height)
                     if pixels[sample_x, y] == SIDEBAR_BLUE]
        row_runs = []
        for y in blue_rows:
            if not row_runs or y != row_runs[-1][-1] + 1:
                row_runs.append([y])
            else:
                row_runs[-1].append(y)
        row_runs = [rows for rows in row_runs if len(rows) >= 80]
        if not row_runs:
            return None
        rows = max(row_runs, key=len)
        top, bottom = rows[0], rows[-1] + 1
        self._installer_sidebar = (frame.width, frame.height, left, top, right, bottom)
        self.sidebar_left, self.sidebar_top = left, top
        self.sidebar_right, self.sidebar_bottom = right, bottom
        self.content_left = right + 24
        return left, top, right, bottom

    def visible_page(self, frame):
        geometry = self._detect_installer_sidebar(frame)
        if geometry is None:
            return None
        _, top, _, bottom = geometry
        right = self.sidebar_right
        sample_left = max(self.sidebar_left + 12, right - 30)
        sample_right = max(sample_left + 1, right - 14)
        sample_left = min(sample_left, frame.width - 1)
        sample_right = min(sample_right, frame.width)
        sample_xs = range(sample_left, sample_right)
        total = len(sample_xs) * 7
        if total <= 0:
            return None
        pixels = frame.load()
        for page in range(SIDEBAR_PAGE_COUNT):
            row_top = top + SIDEBAR_ROW_TOP + page * SIDEBAR_ROW_PITCH
            row_bottom = row_top + SIDEBAR_ROW_HEIGHT
            if row_top < top or row_bottom > bottom or row_bottom > frame.height:
                continue
            y_start = row_top + (SIDEBAR_ROW_HEIGHT - 7) // 2
            count = sum(pixels[x, y] == SIDEBAR_ACTIVE
                        for y in range(y_start, y_start + 7)
                        for x in sample_xs)
            if count >= total * 0.8:
                return page
        return None

    def _require_installer_geometry(self):
        if self._installer_sidebar is None:
            frame = self.screenshot("installer-geometry")
            if self._detect_installer_sidebar(frame) is None:
                raise AssertionError("Installer FLTK sidebar geometry was not detected")
        if self.sidebar_left is None:
            _, _, self.sidebar_left, self.sidebar_top, self.sidebar_right, self.sidebar_bottom = \
                self._installer_sidebar
            self.content_left = self.sidebar_right + 24

    def footer_button_box(self, previous=False):
        self._require_installer_geometry()
        x = self.content_left if previous else self.content_left + 154
        width = 136 if previous else 148
        y = self.sidebar_bottom - 56
        if x < 0 or x + width > self.width or y < 0 or y + 38 > self.height:
            raise AssertionError("Installer footer button is outside the visible screen")
        return x, y, x + width, y + 38

    def footer_point(self, previous=False):
        x1, y1, x2, y2 = self.footer_button_box(previous)
        return (x1 + x2) // 2, (y1 + y2) // 2

    def language_option_point(self, chinese=False):
        self._require_installer_geometry()
        return self.content_left + 200, self.sidebar_top + (191 if chinese else 143)

    def mode_option_point(self, update=False):
        self._require_installer_geometry()
        return self.content_left + 200, self.sidebar_top + (188 if update else 142)

    def disk_selection_point(self, index=0):
        if index != 0:
            raise ValueError("Only the first visible installer disk row is supported")
        self._require_installer_geometry()
        return self.content_left + 100, self.sidebar_top + 144

    def partition_selection_point(self, index=0):
        if index != 0:
            raise ValueError("Only the first visible partition row is supported")
        self._require_installer_geometry()
        return self.content_left + 100, self.sidebar_top + 108

    def account_field_point(self, index):
        if not 0 <= index < 5:
            raise IndexError("Installer account field index must be between 0 and 4")
        self._require_installer_geometry()
        return self.content_left + 305, self.sidebar_top + 137 + index * 82

    def confirm_field_point(self):
        self._require_installer_geometry()
        return self.content_left + 165, self.sidebar_top + 278

    def xorg_indicator_point(self):
        self._require_installer_geometry()
        return self.content_left + 8, self.sidebar_top + 334

    def run(self, max_latency):
        results = []
        observed = set()
        for expected in (1, 2, 3):
            next_x, next_y = self.footer_point()
            self.move(next_x, next_y)
            time.sleep(0.05)
            started = time.monotonic()
            self.button(True)
            release_at = started + 0.06
            released = False
            first_visible = None
            next_frame = started
            index = 0
            # Keep motion arriving more often than the old 100 ms idle wait.
            # The first click must be painted even while that motion continues.
            while time.monotonic() - started < 1.5:
                now = time.monotonic()
                if not released and now >= release_at:
                    self.button(False)
                    released = True
                if released:
                    self.move(500 + index % 100, 450 + index % 50)
                else:
                    self.move(next_x + index % 3, next_y)
                if now >= next_frame:
                    frame = self.screenshot(f"step-{expected}-{index:03d}")
                    visible = self.visible_page(frame)
                    observed.add(visible)
                    if visible == expected and first_visible is None:
                        first_visible = time.monotonic() - started
                    next_frame = time.monotonic() + 0.08
                index += 1
                time.sleep(0.015)
            if not released:
                self.button(False)
            results.append({"page": expected, "latency_ms":
                            round(first_visible * 1000, 1) if first_visible is not None else None})
        time.sleep(0.2)
        final_page = self.visible_page(self.screenshot("settled"))
        report = {"clicks": results, "observed_pages": sorted(
            value for value in observed if value is not None), "final_page": final_page}
        (self.output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report, indent=2), flush=True)
        assert final_page == 3, "Click order/count changed"
        assert all(item["latency_ms"] is not None and
                   item["latency_ms"] <= max_latency * 1000 for item in results), (
            "Installer deferred click feedback while mouse motion continued")

    def close(self):
        self.stream.close()
        self.socket.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("qmp_socket")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--max-latency", type=float, default=0.5)
    args = parser.parse_args()
    probe = InstallerProbe(args.qmp_socket, args.output)
    try:
        probe.run(args.max_latency)
    finally:
        probe.close()


if __name__ == "__main__":
    main()
