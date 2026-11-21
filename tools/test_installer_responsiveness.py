"""Check a running installer's redraw latency through an isolated QEMU QMP socket.

Start the installer at its language page with no writable disk attached. This
test visits only Language, License, Installation mode, and Disk; it never starts a copy.
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
CONTENT_BACKGROUND = (236, 238, 244)
SIDEBAR_ROW_TOP = 100
SIDEBAR_ROW_PITCH = 34
SIDEBAR_ROW_HEIGHT = 30
SIDEBAR_PAGES = (0, 1, 3, 4, 5, 6, 11, 7, 8, 9, 10)
UPDATE_SIDEBAR_PAGES = (0, 1, 3, 4, 8, 9, 10)


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
        self.window_right = None
        self.content_width = None
        self.body_offset = 0
        self._visible_installer_page = None

    def _detect_installer_sidebar(self, frame):
        cached = self._installer_sidebar
        if cached and cached[:2] == frame.size:
            _, _, left, top, right, bottom = cached
            if (0 <= left + 4 < frame.width and 0 <= top + 4 < frame.height and
                    frame.getpixel((left + 4, top + 4)) == SIDEBAR_BLUE and
                    frame.getpixel((left + 4, bottom - 1)) == SIDEBAR_BLUE and
                    (left == 0 or frame.getpixel((left - 1, top + 4)) != SIDEBAR_BLUE) and
                    (top == 0 or frame.getpixel((left + 4, top - 1)) != SIDEBAR_BLUE) and
                    (bottom == frame.height or frame.getpixel((left + 4, bottom)) != SIDEBAR_BLUE)):
                self._detect_window_right(frame, left, top, bottom)
                return left, top, right, bottom

        pixels = frame.load()
        column_counts = []
        # The active white row and sidebar labels interrupt exact-color blue
        # runs inside otherwise full-height columns. A 45% column threshold
        # still distinguishes this tall pane from incidental blue controls.
        for x in range(frame.width):
            count = 0
            for y in range(frame.height):
                if pixels[x, y] == SIDEBAR_BLUE:
                    count += 1
            column_counts.append((x, count))
        maximum = max((count for _, count in column_counts), default=0)
        if maximum < min(240, frame.height * .4):
            return None
        # A 700px installer can occupy only part of a 1080p/4K display. Use
        # the observed pane height, including its thin uninterrupted margins,
        # instead of requiring blue to cover 45% of the entire framebuffer.
        column_threshold = int(maximum * .45)
        blue_columns = [(x, count) for x, count in column_counts if count >= column_threshold]

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
        self._detect_window_right(frame, left, top, bottom)
        return left, top, right, bottom

    def _detect_window_right(self, frame, left, top, bottom):
        # A clear footer margin spans the window's full content width.
        footer_y = max(top, bottom - 72)
        edge = self.content_left
        while edge < frame.width and frame.getpixel((edge, footer_y)) == CONTENT_BACKGROUND:
            edge += 1
        self.window_right = edge if edge > self.content_left + 450 else left + 1024
        self.content_width = self.window_right - self.content_left - 24

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
        sample_height = 4
        total = len(sample_xs) * sample_height
        if total <= 0:
            return None
        pixels = frame.load()
        pages = UPDATE_SIDEBAR_PAGES if getattr(self, "installer_update_mode", False) else SIDEBAR_PAGES
        for row, page in enumerate(pages):
            row_top = top + SIDEBAR_ROW_TOP + row * SIDEBAR_ROW_PITCH
            row_bottom = row_top + SIDEBAR_ROW_HEIGHT
            if row_top < top or row_bottom > bottom or row_bottom > frame.height:
                continue
            # Long labels reach the right edge; sample above their glyphs.
            y_start = row_top + 2
            count = sum(pixels[x, y] == SIDEBAR_ACTIVE
                        for y in range(y_start, y_start + sample_height)
                        for x in sample_xs)
            if count >= total * 0.8:
                if self._visible_installer_page != page:
                    # Every page shares the same fixed content origin.
                    self.body_offset = 0
                    self._visible_installer_page = page
                return page
        # An early partial redraw can otherwise retain incomplete pane bounds.
        self._reset_installer_geometry()
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

    def body_viewport_height(self):
        self._require_installer_geometry()
        return self.sidebar_bottom - self.sidebar_top - 80

    def scroll_body(self, *, bottom=False, content_height=None):
        """Verify that wheel input over the page gutter cannot move the page.

        Keep this entry point for existing QMP callers. Long documents and
        lists may scroll inside their own controls; the surrounding page must
        stay fixed, regardless of the direction or former content height.
        """
        self._require_installer_geometry()
        assert self.body_offset == 0, "Installer page origin changed"
        self.move(self.content_left - 12, self.sidebar_top + 48)
        time.sleep(.1)
        before = self.screenshot("body-before-scroll")
        direction = "wheel-down" if bottom else "wheel-up"
        for _ in range(2):
            self.command("input-send-event", {"events": [
                {"type": "btn", "data": {"button": direction, "down": down}}
                for _ in range(4) for down in (True, False)
            ]})
            time.sleep(.05)
        deadline = time.monotonic() + 8
        while True:
            time.sleep(.2)
            after = self.screenshot("body-after-scroll")
            try:
                self._assert_body_stationary(before, after)
                break
            except AssertionError:
                if time.monotonic() >= deadline:
                    raise
        assert self.body_offset == 0

    def _body_fixed_bands(self, frame):
        """Return fixed page areas around the native scrollable child.

        FLTK may forward an unhandled gutter wheel event to a text display or
        browser. Its contents may scroll, while its surrounding form must not.
        Never exclude acceptance, confirmation or partition editing controls.
        """
        height = self.body_viewport_height()
        page = self._visible_installer_page
        update = getattr(self, "installer_update_mode", False)
        scroll_top = scroll_bottom = None
        if page == 1:
            scroll_top, scroll_bottom = 104, height - 64
        elif page == 4:
            scroll_top, scroll_bottom = 154, height - (216 if update else 94)
        elif page == 6:
            # The manual browser is followed by an 8px clear gap and editing
            # buttons. The automatic browser continues through that gap.
            # Inspect the actual gap, rather than remembering previous clicks.
            gap_y = self.sidebar_top + height - 208
            manual = all(frame.getpixel((self.content_left + x, gap_y)) == CONTENT_BACKGROUND
                         for x in (4, self.content_width // 2, self.content_width - 5))
            scroll_top, scroll_bottom = 164, height - (212 if manual else 100)
        elif page == 8 and not update:
            scroll_top, scroll_bottom = 284, height - 100
        if scroll_top is None or scroll_bottom <= scroll_top:
            return [(0, height)]
        return [(0, scroll_top), (scroll_bottom, height)]

    def _assert_body_stationary(self, before, after):
        left, right = self.content_left, self.content_left + self.content_width
        top = self.sidebar_top
        foreground_count = 0
        for start, end in self._body_fixed_bands(before):
            original = before.crop((left, top + start, right, top + end))
            current = after.crop((left, top + start, right, top + end))
            # Check each fixed area separately: a large unchanged header must
            # not hide a moved acceptance checkbox or confirmation field.
            # Allow a blinking caret, but require actual foreground evidence.
            foreground = [(a, b) for a, b in zip(original.getdata(), current.getdata())
                          if a != CONTENT_BACKGROUND or b != CONTENT_BACKGROUND]
            foreground_count += len(foreground)
            same = sum(a == b for a, b in foreground)
            assert same >= len(foreground) * .98, \
                (f"Installer page moved after gutter wheel input: fixed rows {start}:{end}, "
                 f"{same}/{len(foreground)} foreground pixels match")
        assert foreground_count >= 100, "Insufficient visible content to verify a fixed page"

    def body_point(self, x, y, *, content_height=None, margin=18):
        self._require_installer_geometry()
        assert self.body_offset == 0, "Installer page origin changed"
        viewport = self.body_viewport_height()
        if not (0 <= x < self.content_width and 0 <= y < viewport):
            raise AssertionError(f"Installer body point ({x}, {y}) is outside the visible page")
        return self.content_left + x, self.sidebar_top + y

    def language_option_point(self, chinese=False):
        self._require_installer_geometry()
        return self.content_left + 200, self.sidebar_top + (191 if chinese else 143)

    def mode_option_point(self, update=False):
        self._require_installer_geometry()
        return self.content_left + 200, self.sidebar_top + (260 if update else 142)

    def disk_selection_point(self, index=0):
        if index != 0:
            raise ValueError("Only the first visible installer disk row is supported")
        self._require_installer_geometry()
        return self.body_point(100, 166)

    def partition_browser_box(self, *, manual=True):
        """Bounds of the native browser, excluding the page's edit controls."""
        return (0, 164, self.content_width,
                self.body_viewport_height() - (212 if manual else 100))

    def partition_method_point(self, *, manual=True):
        return self.body_point(200, 96 if manual else 64)

    def partition_format_indicator_point(self):
        return self.body_point(8, self.body_viewport_height() - 114)

    def update_activity_box(self):
        return 0, 202, self.content_width, 284

    def partition_selection_point(self, index=0):
        if index != 0:
            raise ValueError("Only the first visible partition row is supported")
        self._require_installer_geometry()
        return self.body_point(100, 176)

    def bootloader_indicator_point(self, bios=True):
        self._require_installer_geometry()
        return self.content_left + 8, self.sidebar_top + (154 if bios else 198)

    def account_field_point(self, index):
        if not 0 <= index < 5:
            raise IndexError("Installer account field index must be between 0 and 4")
        self._require_installer_geometry()
        column = (self.content_width - 24) // 2
        x = column + 24 + column // 2 if index >= 3 else \
            self.content_width // 2 if index == 0 else column // 2
        y = 117 if index == 0 else 237 if index in (1, 3) else 317
        return self.body_point(x, y)

    def confirm_field_point(self):
        self._require_installer_geometry()
        return self.body_point(self.content_width // 4, self.body_viewport_height() - 32)

    def license_indicator_point(self):
        self._require_installer_geometry()
        return self.body_point(8, self.body_viewport_height() - 31)

    def desktop_option_point(self):
        self._require_installer_geometry()
        return self.body_point(self.content_width // 2, 169)

    def swap_indicator_point(self):
        self._require_installer_geometry()
        return self.body_point(8, 284)

    def swap_size_point(self):
        self._require_installer_geometry()
        return self.body_point(self.content_width // 4, 367)

    def xorg_indicator_point(self):
        self._require_installer_geometry()
        return self.content_left + 8, self.sidebar_top + 334

    def run(self, max_latency):
        results = []
        observed = set()
        for expected in (1, 3, 4):
            if expected == 3:
                self.move(*self.license_indicator_point())
                self.button(True)
                time.sleep(0.06)
                self.button(False)
                time.sleep(0.1)
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
        assert final_page == 4, "Click order/count changed"
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
