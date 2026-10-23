"""Stateful GUI checks for the FLTK Task Manager acceptance harness.

The caller supplies the running QMP probe, app client geometry, the harness
module as ``support``, and the serial log.  This module deliberately uses OCR
and the fixture worker's exact PID before invoking any task-ending action.
"""
from __future__ import annotations

import csv
import io
import re
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Any
from PIL import Image, ImageOps


_PROCESS_LIST_X = 20
_PROCESS_LIST_TOP = 132
_PROCESS_LIST_BOTTOM_INSET = 48
_PROCESS_HEADER_TOP = 108
_PROCESS_TOOLBAR = (20, 66, 296, 30)
_REFRESH_BUTTON = (20, 66, 88, 30)
_PERFORMANCE_VIEW = (20, 66)
_STARTUP_USER_TOP = 66
_STARTUP_LIST_TOP = 148
_PROCESS_NAME_WIDTHS = (80, 64, 96, 84, 110, 90)
_PROCESS_COLUMN_WIDTHS = (80, 64, 96, 84, 110, 90)
# The sleep applet in this guest resolves to the BusyBox executable.
# Match its real snapshot name as well as the unique serial-provided PID.
_WORKER_NAME = "busybox"
_DETAILS_CHINESE_FIELDS = ("父 PID", "类型", "用户", "权限", "CPU 时间", "内存", "唤醒 tick")


def _client(support: Any, frame: Any, geometry: tuple[int, int, int, int]):
    return support.client_crop(frame, *geometry)


def _save_client(support: Any, frame: Any, geometry: tuple[int, int, int, int],
                 output: Path, tag: str) -> str:
    path = output / f"{tag}.client.png"
    _client(support, frame, geometry).save(path)
    return str(path)


def _ocr(support: Any, image: Any, output: Path, tag: str, psm: int = 11,
         scale: int = 2) -> str:
    return support.tesseract_ocr(_readable_ocr_image(image), output, tag, psm=psm,
                                 scale=scale)


def _readable_ocr_image(image: Any):
    """Normalize blue selected rows for OCR; preserve the raw screenshots."""
    rgb = image.convert("RGB")
    raw = rgb.tobytes()
    gray = bytearray(ImageOps.grayscale(rgb).tobytes())
    for y in range(rgb.height):
        line = raw[y * rgb.width * 3:(y + 1) * rgb.width * 3]
        blue = sum(b > r + 32 and b > g + 32
                   for r, g, b in zip(line[::3], line[1::3], line[2::3]))
        if blue > rgb.width // 2:
            first = y * rgb.width
            gray[first:first + rgb.width] = bytes(255 - value for value in gray[first:first + rgb.width])
    return Image.frombytes("L", rgb.size, bytes(gray))


def _normalized(support: Any, text: str, expected: str) -> bool:
    # SimSun's serif capital I in the fixed PID label is often read as 1.
    # Normalize that label only; never substitute digits in PID values.
    if "PID" in expected:
        compact = re.sub(r"[\s\W_]+", "", text, flags=re.UNICODE).lower()
        compact = compact.replace("p1d", "pid").replace("pld", "pid")
        return support.normalized_contains(compact, expected)
    return support.normalized_contains(text, expected)


def _serial_text(serial: Path) -> str:
    return serial.read_text(errors="replace") if serial.exists() else ""


def _worker_pid_from_serial(serial: Path) -> int:
    matches = re.findall(r"(?m)^FLTK_TASKMGR_WORKER pid=(\d+)\s*$", _serial_text(serial))
    if not matches:
        raise AssertionError("serial log omitted FLTK_TASKMGR_WORKER pid marker")
    return int(matches[-1])


def _tsv_words(image: Any, *, language: str = "eng", psm: int = 6) -> list[dict[str, str]]:
    """Return Tesseract TSV word records, scaled back to the source image."""
    image = _readable_ocr_image(image)
    resized = image.resize((max(1, image.width * 2), max(1, image.height * 2)))
    with tempfile.NamedTemporaryFile(prefix="fltk-taskmgr-", suffix=".png") as temporary:
        resized.save(temporary.name)
        raw = subprocess.check_output(
            ["tesseract", temporary.name, "stdout", "-l", language,
             "--psm", str(psm), "tsv"],
            text=True, stderr=subprocess.DEVNULL)
    reader = csv.DictReader(io.StringIO(raw), delimiter="\t")
    words = []
    for row in reader:
        if row.get("level") != "5" or not row.get("text", "").strip():
            continue
        try:
            row["_left"] = str(float(row["left"]) / 2.0)
            row["_top"] = str(float(row["top"]) / 2.0)
            row["_width"] = str(float(row["width"]) / 2.0)
            row["_height"] = str(float(row["height"]) / 2.0)
            row["_confidence"] = str(float(row["conf"]))
        except (KeyError, TypeError, ValueError):
            continue
        words.append(row)
    return words


def _process_columns(window_width: int) -> list[tuple[int, int]]:
    """Compute the process header/data columns from TaskWindow::layout_ui."""
    list_width = window_width - 40
    available = max(1, list_width - 20)
    name_width = max(150, available - sum(_PROCESS_NAME_WIDTHS))
    widths = (name_width,) + _PROCESS_COLUMN_WIDTHS
    x = _PROCESS_LIST_X + 5
    result = []
    for width in widths:
        result.append((x, width))
        x += width
    return result


def _process_list_bottom(window_height: int) -> int:
    return max(_PROCESS_LIST_TOP + 1,
               window_height - _PROCESS_LIST_BOTTOM_INSET)


def _gpu_card_geometry(window_width: int) -> tuple[int, int, int]:
    """Return the GPU graph's client-relative x, width, and title y."""
    view_width = max(1, window_width - 58)  # scroll width minus its 18px bar
    content_width = max(1, view_width - 24)
    columns = 3 if content_width >= 700 else 1
    if columns != 3:
        raise AssertionError("GPU graph layout changed below the supported window minimum")
    graph_width = (content_width - 8 * (columns - 1)) // columns
    # The app minimum is 800px wide, so its three performance graphs share
    # the first row at both the default and minimum layouts.
    return (28 + 2 * (graph_width + 8), graph_width,
            _PERFORMANCE_VIEW[1] + 8)


def _performance_regions(window_width: int, window_height: int,
                         cpu_count: int = 2) -> dict[str, Any]:
    """Compute compact performance OCR crops from TaskWindow::layout_ui.

    The QEMU acceptance VM uses two vCPUs.  These formulas mirror
    ``update_performance_layout`` and ``draw_performance`` so the checks stay
    anchored to the widget geometry if the app is resized.
    """
    viewport_width = max(1, window_width - 40)
    viewport_height = max(1, window_height - 114)
    view_width = max(1, viewport_width - 18)  # Fl_Scroll's vertical bar
    content_width = max(1, view_width - 24)
    columns = 3 if content_width >= 700 else 1
    gap = 8
    graph_width = max(1, (content_width - gap * (columns - 1)) // columns)

    core_columns = 3 if cpu_count > 16 else (2 if cpu_count > 8 else 1)
    if content_width < 200:
        core_columns = 1
    core_rows = max(1, (cpu_count + core_columns - 1) // core_columns)

    # Match update_performance_layout's two-pass content-height calculation,
    # then use draw_performance's graph height for the current column count.
    layout_graph_height = 126 if viewport_height > 440 else 100
    required_height = layout_graph_height + 168 + core_rows * 27
    child_height = max(viewport_height, required_height)
    layout_graph_height = 126 if child_height > 440 else 100
    required_height = layout_graph_height + 168 + core_rows * 27
    child_height = max(viewport_height, required_height)
    graph_height = (92 if columns == 1 else
                    (126 if child_height > 440 else 100))

    widget_x, widget_y = _PERFORMANCE_VIEW
    graph_top = widget_y + 8
    cards = []
    for index in range(3):
        cards.append((
            widget_x + 8 + (index % columns) * (graph_width + gap),
            graph_top + (index // columns) * (graph_height + gap),
            graph_width,
        ))

    info_y = graph_top + graph_height + gap + 20
    core_top = info_y + 26
    column_width = content_width // core_columns
    core_rows_geometry = []
    for index in range(cpu_count):
        row_y = core_top + (index // core_columns) * 27
        column_x = widget_x + 24 + (index % core_columns) * column_width
        core_rows_geometry.append({
            "label": (column_x + 8, row_y - 2, 96, 22),
            "value": (column_x + column_width - 58, row_y - 2, 52, 22),
        })

    stats_y = (core_top + core_rows * 27 + 5)
    return {
        "cards": cards,
        "per_core_title": (widget_x + 18, info_y - 18, 400, 30),
        "per_core_rows": core_rows_geometry,
        "stats": (widget_x + 18, stats_y - 2, 400, 120),
        "child_height": child_height,
    }


def _percent_reading(text: str) -> int | None:
    match = re.search(r"(?<!\d)(\d{1,3})\s*%", text)
    if not match:
        return None
    value = int(match.group(1))
    return value if value <= 100 else None


def _performance_fields(support: Any, frame: Any,
                        geometry: tuple[int, int, int, int], output: Path,
                        tag: str, cpu_count: int = 2) -> dict[str, Any]:
    """OCR performance labels and values in small, source-aligned regions."""
    x, y, width, height = geometry
    regions = _performance_regions(width, height, cpu_count)
    screenshot_paths: dict[str, str] = {}

    def crop_ocr(name: str, rect: tuple[int, int, int, int], psm: int = 7) -> str:
        region = support.client_crop(frame, x + rect[0], y + rect[1],
                                     rect[2], rect[3])
        path = output / f"{tag}-{name.replace('_', '-')}.png"
        region.save(path)
        screenshot_paths[name] = str(path)
        return _ocr(support, region, output, f"{tag}-{name}", psm=psm)

    cpu_x, cpu_y, graph_width = regions["cards"][0]
    memory_x, memory_y, memory_width = regions["cards"][1]
    cpu_title = crop_ocr("cpu-title", (cpu_x + 4, cpu_y,
                                       max(1, graph_width - 80), 24))
    cpu_value_text = crop_ocr("cpu-value", (cpu_x + graph_width - 72, cpu_y,
                                            68, 24))
    memory_title = crop_ocr("memory-title", (memory_x + 4, memory_y,
                                             max(1, memory_width - 80), 24))
    memory_value_text = crop_ocr("memory-value", (
        memory_x + memory_width - 72, memory_y, 68, 24))
    per_core_title = crop_ocr("per-core-title", regions["per_core_title"])

    core_texts: dict[str, str] = {}
    core_values: dict[str, int | None] = {}
    core_labels: dict[str, bool] = {}
    for index, row in enumerate(regions["per_core_rows"]):
        label_text = crop_ocr(f"core-{index}-label", row["label"])
        value_text = crop_ocr(f"core-{index}-value", row["value"])
        core_texts[f"cpu_{index}"] = value_text.strip()
        core_values[f"cpu_{index}"] = _percent_reading(value_text)
        core_labels[f"cpu_{index}"] = _normalized(
            support, label_text, f"CPU {index}")

    stats_text = crop_ocr("memory-uptime-stats", regions["stats"], psm=6)
    stat_specs = {
        "total_memory": ("总内存", r"(?<!\d)([\d,]+\s*(?:KiB|KB|MiB|MB|GiB|GB))\b"),
        "used_memory": ("已用内存", r"(?<!\d)([\d,]+\s*(?:KiB|KB|MiB|MB|GiB|GB))\b"),
        "free_memory": ("可用内存", r"(?<!\d)([\d,]+\s*(?:KiB|KB|MiB|MB|GiB|GB))\b"),
        "uptime": ("运行时间", r"(?<!\d)(\d+\s*:\s*\d{2}\s*:\s*\d{2})(?!\d)"),
    }
    stats_labels: dict[str, bool] = {}
    stats_values: dict[str, str | None] = {}
    stat_lines = [line.strip() for line in stats_text.splitlines() if line.strip()]
    for key, (label, value_pattern) in stat_specs.items():
        matching_line = next(
            (line for line in stat_lines if _normalized(support, line, label)), "")
        value_match = re.search(value_pattern, matching_line, re.IGNORECASE)
        stats_labels[key] = bool(matching_line)
        stats_values[key] = value_match.group(1).strip() if value_match else None

    values = {
        "cpu_percent": _percent_reading(cpu_value_text),
        "memory_percent": _percent_reading(memory_value_text),
        **{f"{name}_percent": value for name, value in core_values.items()},
        **stats_values,
    }
    labels = {
        "cpu": _normalized(support, cpu_title, "CPU 占用"),
        "memory": _normalized(support, memory_title, "内存占用"),
        "per_core": _normalized(support, per_core_title, "每核占用"),
        **stats_labels,
        **{f"{name}_label": valid for name, valid in core_labels.items()},
    }
    value_checks = {
        "cpu_percent": values["cpu_percent"] is not None,
        "memory_percent": values["memory_percent"] is not None,
        **{f"{name}_percent": value is not None
           for name, value in core_values.items()},
        **{key: value is not None for key, value in stats_values.items()},
    }
    texts = {
        "cpu_title": cpu_title.strip(),
        "cpu_value": cpu_value_text.strip(),
        "memory_title": memory_title.strip(),
        "memory_value": memory_value_text.strip(),
        "per_core_title": per_core_title.strip(),
        "per_core_values": core_texts,
        "stats": stats_text.strip(),
    }
    return {
        "labels": labels,
        "values": values,
        "value_checks": value_checks,
        "texts": texts,
        "screenshots": screenshot_paths,
    }


def _startup_regions(window_width: int) -> dict[str, tuple[int, int, int, int]]:
    """Return inset label crops for the startup controls from layout_ui."""
    # Inset each crop from the button frame so disabled-state bevels do not
    # enter OCR; retain the complete centered label area.
    return {
        "enable": (26, 110, 84, 22),
        "remove": (135, 111, 74, 20),
        "tab_strip": (12, 34, max(1, window_width - 24), 22),
    }


def _pid_in_visible_rows(support: Any, frame: Any,
                         geometry: tuple[int, int, int, int], pid: int
                         ) -> tuple[int | None, tuple[str, ...]]:
    """Find an exact PID token in the PID column and return its client y."""
    _, _, width, height = geometry
    columns = _process_columns(width)
    pid_x, pid_width = columns[1]
    # Add a few pixels on both sides for FLTK's browser text inset.  The
    # resulting region stays inside the PID column at the minimum window size.
    left = max(_PROCESS_LIST_X, pid_x - 5)
    crop_width = min(pid_width + 10, width - left - 4)
    list_height = min(_process_list_bottom(height) - _PROCESS_LIST_TOP,
                      height - _PROCESS_LIST_TOP - 4)
    crop = support.client_crop(
        frame, geometry[0] + left, geometry[1] + _PROCESS_LIST_TOP,
        crop_width, list_height)
    # A native dotted focus rectangle spans the PID column and can join its
    # neighboring text lines in OCR. Digits occupy much less than 40% of a row.
    gray = _readable_ocr_image(crop)
    pixels = bytearray(gray.tobytes())
    for y in range(gray.height):
        first = y * gray.width
        if sum(value < 100 for value in pixels[first:first + gray.width]) > gray.width * 0.4:
            pixels[first:first + gray.width] = bytes([255]) * gray.width
    crop = Image.frombytes("L", gray.size, bytes(pixels))
    words = _tsv_words(crop, language="eng", psm=6)
    target = str(pid)
    y_values = []
    visible_pids = []
    for word in words:
        token = word["text"].strip()
        numeric = re.fullmatch(r"[\W_]*(\d+)[\W_]*", token)
        digits = numeric.group(1) if numeric else ""
        if digits:
            visible_pids.append(digits)
        if digits == target and float(word["_confidence"]) >= 10:
            center_y = _PROCESS_LIST_TOP + round(
                float(word["_top"]) + float(word["_height"]) / 2.0)
            y_values.append(center_y)
    return (y_values[0] if y_values else None, tuple(visible_pids))


def _row_text(support: Any, frame: Any,
              geometry: tuple[int, int, int, int], row_y: int,
              output: Path, tag: str) -> tuple[str, str]:
    x, y, width, height = geometry
    top = max(_PROCESS_LIST_TOP, row_y - 11)
    bottom = min(_process_list_bottom(height), height - 3, row_y + 12)
    row = support.client_crop(frame, x + _PROCESS_LIST_X + 2,
                              y + top, width - _PROCESS_LIST_X - 24,
                              max(1, bottom - top))
    path = output / f"{tag}.png"
    row.save(path)
    return _ocr(support, row, output, tag, psm=7), str(path)


def _row_cell_text(support: Any, frame: Any,
                   geometry: tuple[int, int, int, int], row_y: int,
                   column: tuple[int, int], output: Path,
                   tag: str) -> str:
    x, y, _, height = geometry
    top = max(_PROCESS_LIST_TOP, row_y - 11)
    bottom = min(_process_list_bottom(height), height - 3, row_y + 12)
    column_x, column_width = column
    cell = support.client_crop(frame, x + column_x, y + top,
                               max(1, column_width), max(1, bottom - top))
    cell.save(output / f"{tag}.png")
    return _ocr(support, cell, output, tag, psm=7)


def _exact_pid_and_name(support: Any, text: str, pid: int, name: str) -> bool:
    # Keep punctuation and line boundaries: deleting ')' could merge a PID
    # with OCR noise from the following question mark. Do not accept Parent PID.
    for line in text.splitlines():
        compact = re.sub(r"\s+", "", line).lower()
        compact = compact.replace("p1d", "pid").replace("pld", "pid")
        match = re.search(r"(?<!\w)pid[:：]?(\d+)(?!\d)", compact)
        if match and int(match.group(1)) == pid:
            return _normalized(support, text, name)
    return False


def _active_dialog_title(frame: Any):
    """Locate the active native modal title bar above its inactive parent."""
    rgb = frame.convert("RGB")
    for y in range(40, frame.height - 100):
        start = None
        for x in range(frame.width):
            r, g, b = rgb.getpixel((x, y))
            blue = b > r + 32 and b > g + 32
            if blue and start is None:
                start = x
            if not blue and start is not None:
                # IceWM's active title bar precedes the selected table row.
                # The Details title is shorter than a process-list selection.
                if 120 <= x - start <= 700:
                    return start, y, x
                start = None
    raise AssertionError("active native dialog title bar was not visible")


def _details_text_crop(frame: Any):
    left, top, right = _active_dialog_title(frame)
    return frame.crop((left + 68, top + 26,
                       min(frame.width, right + 34), top + 238))


def _capture_page(probe: Any, support: Any,
                  geometry: tuple[int, int, int, int], output: Path,
                  tag: str, psm: int = 11) -> tuple[Any, Any, str, str]:
    probe.move(geometry[0] + geometry[2] - 24, geometry[1] + geometry[3] - 12)
    time.sleep(0.1)
    frame = probe.frame(tag)
    client = _client(support, frame, geometry)
    client_path = output / f"{tag}.client.png"
    client.save(client_path)
    ocr_text = _ocr(support, client, output, tag, psm=psm)
    return frame, client, ocr_text, str(client_path)


def exercise_taskmgr(probe: Any, geometry: tuple[int, int, int, int],
                     serial: Path, process: Any, output: Path,
                     failures: list[str], taskmgr_pid: int,
                     support: Any) -> dict[str, Any]:
    """Exercise Task Manager pages and safely end only the verified worker.

    ``support`` is the running ``test_fltk_apps_qemu`` module.  All evidence
    paths and values returned here are JSON-serializable.
    """
    output = Path(output)
    evidence: dict[str, Any] = {
        "performed": True,
        "passed": False,
        "checks": {},
        "screenshots": {},
    }
    checks: dict[str, Any] = evidence["checks"]
    screenshots: dict[str, str] = evidence["screenshots"]
    modal_may_be_open = False
    try:
        worker_pid = _worker_pid_from_serial(Path(serial))
        if worker_pid <= 1 or worker_pid == int(taskmgr_pid):
            raise AssertionError(
                f"unsafe worker PID marker: worker={worker_pid}, taskmgr={taskmgr_pid}")
        checks["worker_pid"] = worker_pid
        checks["taskmgr_pid"] = int(taskmgr_pid)

        # The compact process page has no repeated CPU/memory summary. Verify
        # its real action row and all seven independently aligned columns.
        frame, _, _, client_path = _capture_page(
            probe, support, geometry, output, "app-taskmgr-processes", psm=11)
        screenshots["processes"] = client_path
        x, y, width, height = geometry
        toolbar_x, toolbar_y, toolbar_w, toolbar_h = _PROCESS_TOOLBAR
        toolbar = support.client_crop(
            frame, x + toolbar_x, y + toolbar_y, toolbar_w, toolbar_h)
        toolbar_path = output / "app-taskmgr-process-toolbar.png"
        toolbar.save(toolbar_path)
        toolbar_text = "\n".join(
            _ocr(support, support.client_crop(frame, x + button_x + 8, y + 71,
                                              button_width - 16, 20), output,
                 f"app-taskmgr-process-toolbar-{button_x}", psm=7)
            for button_x, button_width in ((20, 88), (112, 96), (212, 104)))
        toolbar_labels = {
            "refresh": _normalized(support, toolbar_text, "刷新"),
            "details": _normalized(support, toolbar_text, "详细信息"),
            "end_task": _normalized(support, toolbar_text, "结束任务"),
        }
        if not all(toolbar_labels.values()):
            raise AssertionError(
                f"process toolbar actions were missing: {toolbar_labels}; OCR={toolbar_text!r}")
        screenshots["process_toolbar"] = str(toolbar_path)
        checks["process_toolbar_actions"] = toolbar_labels

        columns = _process_columns(width)
        labels = ("Process", "PID", "CPU", "Memory", "State", "User", "Privileges")
        translated_labels = ("进程", "PID", "CPU", "内存", "状态", "用户", "权限")
        header_y = y + _PROCESS_HEADER_TOP
        header_results: dict[str, bool] = {}
        for index, ((column_x, column_width), label, translated) in enumerate(
                zip(columns, labels, translated_labels)):
            crop = support.client_crop(
                frame, x + column_x, header_y, max(1, column_width), 22)
            tag = f"app-taskmgr-header-{index}"
            crop_path = output / f"{tag}.png"
            crop.save(crop_path)
            header_ocr = _ocr(support, crop, output, tag, psm=7)
            header_results[label] = (
                _normalized(support, header_ocr, label) or
                _normalized(support, header_ocr, translated))
            screenshots[f"header_{index}"] = str(crop_path)
        if not all(header_results.values()):
            raise AssertionError(f"process column labels were missing or misaligned: {header_results}")
        checks["aligned_process_column_labels"] = header_results

        # Search the visible PID column with TSV coordinates.  Page Down is
        # used only to reveal another viewport; the action target is selected
        # by clicking the row that contains the exact serial-provided PID.
        probe.click(x + _PROCESS_LIST_X + 24, y + _PROCESS_LIST_TOP + 10)
        probe.key("home")
        matching_frame = None
        worker_row_y = None
        previous_signature: tuple[str, ...] | None = None
        pages_scanned = 0
        for page in range(17):
            probe.move(x + width - 24, y + height - 12)
            current = probe.frame(f"app-taskmgr-processes-scan-{page}")
            found_y, signature = _pid_in_visible_rows(
                support, current, geometry, worker_pid)
            pages_scanned += 1
            if found_y is not None:
                matching_frame = current
                worker_row_y = found_y
                break
            if page == 16 or (previous_signature is not None and signature == previous_signature):
                break
            previous_signature = signature
            try:
                probe.key("pgdn")
            except Exception:
                # Older QEMU builds may expose Page Down under this alias.
                probe.key("pagedown")
            time.sleep(0.15)
        if matching_frame is None or worker_row_y is None:
            raise AssertionError(
                f"fixture worker PID {worker_pid} was not visible in the process list "
                f"after scanning {pages_scanned} viewport(s)")
        row_ocr, row_path = _row_text(
            support, matching_frame, geometry, worker_row_y, output,
            "app-taskmgr-worker-row")
        if (not _normalized(support, row_ocr, _WORKER_NAME) or
                not _normalized(support, row_ocr, str(worker_pid))):
            raise AssertionError(
                f"PID {worker_pid} row did not identify the sleep worker: {row_ocr!r}")
        cpu_text = _row_cell_text(
            support, matching_frame, geometry, worker_row_y, columns[2],
            output, "app-taskmgr-worker-cpu-cell")
        memory_text = _row_cell_text(
            support, matching_frame, geometry, worker_row_y, columns[3],
            output, "app-taskmgr-worker-memory-cell")
        if not re.search(r"\d+\s*%", cpu_text):
            raise AssertionError(f"worker CPU value was not aligned under CPU: {cpu_text!r}")
        if not re.search(r"\d+(?:\.\d+)?\s*(?:B|KB|MB|GB|TB)",
                         memory_text, re.IGNORECASE):
            raise AssertionError(f"worker memory value was not aligned under Memory: {memory_text!r}")
        screenshots["worker_row"] = row_path
        checks["worker_pid_row_exact"] = True
        checks["worker_name_in_same_row"] = _WORKER_NAME
        checks["worker_cpu_column_value"] = cpu_text.strip()
        checks["worker_memory_column_value"] = memory_text.strip()
        checks["process_viewports_scanned"] = pages_scanned

        columns = _process_columns(width)
        pid_x, pid_width = columns[1]
        probe.click(x + pid_x + pid_width // 2, y + worker_row_y)
        time.sleep(0.2)
        # Enter is the documented Details action for a selected process row.
        modal_may_be_open = True
        probe.key("ret")
        time.sleep(0.3)
        details_frame = probe.frame("app-taskmgr-worker-details")
        details_path = output / "app-taskmgr-worker-details.png"
        details_frame.save(details_path)
        screenshots["details"] = str(details_path)
        details_crop = _details_text_crop(details_frame)
        details_crop_path = output / "app-taskmgr-worker-details-text.png"
        details_crop.save(details_crop_path)
        screenshots["details_text"] = str(details_crop_path)
        details_text = _ocr(
            support, details_crop, output, "app-taskmgr-worker-details-ocr", psm=6)
        if not _exact_pid_and_name(support, details_text, worker_pid, _WORKER_NAME):
            raise AssertionError(
                f"Details did not show the exact fixture PID/name {worker_pid}/{_WORKER_NAME}: "
                f"{details_text!r}")
        details_fields = {
            field: _normalized(support, details_text, field)
            for field in _DETAILS_CHINESE_FIELDS
        }
        if sum(details_fields.values()) < 5:
            raise AssertionError(
                f"Details modal omitted Chinese task fields: {details_fields}; "
                f"OCR={details_text!r}")
        checks["details_pid_and_name"] = True
        checks["details_chinese_fields"] = details_fields
        # Distinct details-only fields prove the modal really opened.
        checks["details_modal_confirmed"] = all(
            details_fields[field] for field in ("类型", "CPU 时间", "唤醒 tick"))
        if not checks["details_modal_confirmed"]:
            raise AssertionError(f"Details modal evidence was incomplete: {details_fields}")

        probe.key("esc")
        modal_may_be_open = False
        time.sleep(0.25)

        # Re-find the exact row after returning from Details. This guards the
        # Move focus off the browser with its Refresh button. Do not infer a
        # selection change from the first row or depend on OCR of Taskmgr's
        # own PID: a focused native dotted outline can obscure an adjacent PID.
        refresh_button = support.client_crop(
            probe.frame("app-taskmgr-before-refresh"),
            x + _REFRESH_BUTTON[0] + 8, y + _REFRESH_BUTTON[1] + 5,
            _REFRESH_BUTTON[2] - 16, _REFRESH_BUTTON[3] - 10)
        refresh_button_path = output / "app-taskmgr-before-reselect-refresh-button.png"
        refresh_button.save(refresh_button_path)
        refresh_text = _ocr(
            support, refresh_button, output,
            "app-taskmgr-before-reselect-refresh-button", psm=7)
        if not _normalized(support, refresh_text, "刷新"):
            raise AssertionError(
                f"Refresh button was not verified before re-finding worker: {refresh_text!r}")
        screenshots["before_reselect_refresh_button"] = str(refresh_button_path)
        probe.click(x + _REFRESH_BUTTON[0] + _REFRESH_BUTTON[2] // 2,
                    y + _REFRESH_BUTTON[1] + _REFRESH_BUTTON[3] // 2)
        time.sleep(0.25)
        probe.move(x + width - 24, y + height - 12)
        time.sleep(0.1)
        current = probe.frame("app-taskmgr-before-reselect")
        screenshots["before_reselect"] = _save_client(
            support, current, geometry, output, "app-taskmgr-before-reselect")
        row_y, _ = _pid_in_visible_rows(support, current, geometry, worker_pid)
        if row_y is None:
            raise AssertionError(f"fixture PID {worker_pid} disappeared before End Task")
        row_ocr, row_path = _row_text(
            support, current, geometry, row_y, output,
            "app-taskmgr-worker-row-before-end")
        if (not _normalized(support, row_ocr, _WORKER_NAME) or
                not _normalized(support, row_ocr, str(worker_pid))):
            raise AssertionError(
                f"End Task target row no longer matches PID/name: {row_ocr!r}")
        screenshots["worker_row_before_end"] = row_path
        pid_x, pid_width = _process_columns(width)[1]
        probe.click(x + pid_x + pid_width // 2, y + row_y)
        time.sleep(0.2)
        modal_may_be_open = True
        probe.key("delete")
        probe.move(x + width - 24, y + height - 12)
        time.sleep(0.3)

        confirm_frame = probe.frame("app-taskmgr-end-confirmation")
        confirm_path = output / "app-taskmgr-end-confirmation.png"
        confirm_frame.save(confirm_path)
        screenshots["end_confirmation"] = str(confirm_path)
        dialog_left, dialog_top, dialog_right = _active_dialog_title(confirm_frame)
        dialog_crop = confirm_frame.crop((dialog_left, dialog_top + 21,
                                         dialog_right + 36, dialog_top + 105))
        dialog_crop.save(output / "app-taskmgr-end-confirmation-dialog.png")
        confirmation_text = _ocr(
            support, dialog_crop, output,
            "app-taskmgr-end-confirmation-ocr", psm=6)
        if (not _normalized(support, confirmation_text, "结束任务") or
                not _exact_pid_and_name(support, confirmation_text, worker_pid, _WORKER_NAME)):
            raise AssertionError(
                f"confirmation did not name only the verified worker PID {worker_pid}: "
                f"{confirmation_text!r}")
        checks["end_confirmation_pid_verified"] = worker_pid
        checks["end_confirmation_action"] = "Enter: native default Yes button"

        marker_offset = len(_serial_text(Path(serial)))
        probe.key("ret")
        modal_may_be_open = False
        exit_marker = f"FLTK_TASKMGR_WORKER_EXIT pid={worker_pid} status="
        serial_after = support.wait_marker(
            Path(serial), exit_marker, process, marker_offset, 20)
        exit_line = next((line for line in serial_after[marker_offset:].splitlines()
                          if line.startswith(exit_marker)), "")
        exit_match = re.search(
            rf"^FLTK_TASKMGR_WORKER_EXIT pid={worker_pid} status=(-?\d+)\s*$",
            exit_line)
        if not exit_match:
            raise AssertionError(f"worker exit marker did not preserve exact PID/status: {exit_line!r}")
        exit_status = int(exit_match.group(1))
        if exit_status == 0:
            raise AssertionError(
                f"fixture worker {worker_pid} exited normally instead of through Task Manager")
        checks["worker_exit_marker"] = exit_line
        checks["worker_exit_status_nonzero"] = True

        # Let Task Manager's 500ms timer refresh the list, then verify the
        # exact worker PID is absent from the same visible viewport.
        after_end_frame = None
        remaining_y = None
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            time.sleep(0.55)
            after_end_frame = probe.frame("app-taskmgr-after-end")
            remaining_y, _ = _pid_in_visible_rows(
                support, after_end_frame, geometry, worker_pid)
            if remaining_y is None:
                break
        if after_end_frame is None:
            raise AssertionError("Task Manager did not produce an after-end frame")
        after_end_path = _save_client(
            support, after_end_frame, geometry, output, "app-taskmgr-after-end")
        screenshots["after_end"] = after_end_path
        if remaining_y is not None:
            raise AssertionError(
                f"Task Manager still displayed fixture PID {worker_pid} after its exit marker")
        checks["worker_removed_from_process_list"] = True

        # Performance has its own page and live 500ms sampling timer.  Verify
        # unique page labels and numeric readings both before and after a tick.
        probe.click(x + 90, y + 47)
        time.sleep(0.25)
        perf_frame, _, _, perf_path = _capture_page(
            probe, support, geometry, output, "app-taskmgr-performance", psm=11)
        screenshots["performance"] = perf_path
        perf_fields = _performance_fields(
            support, perf_frame, geometry, output, "app-taskmgr-performance-fields")
        screenshots.update({f"performance_{name}": path
                            for name, path in perf_fields["screenshots"].items()})
        gpu_x, graph_width, gpu_y = _gpu_card_geometry(width)
        gpu_title_crop = support.client_crop(
            perf_frame, x + gpu_x, y + gpu_y,
            max(1, graph_width - 80), 24)
        gpu_title_path = output / "app-taskmgr-gpu-title.png"
        gpu_title_crop.save(gpu_title_path)
        gpu_title_text = _ocr(
            support, gpu_title_crop, output, "app-taskmgr-gpu-title", psm=7)
        gpu_value = support.client_crop(
            perf_frame, x + gpu_x + graph_width - 68, y + gpu_y, 68, 24)
        screenshots["gpu_title"] = str(gpu_title_path)
        gpu_value.save(output / "app-taskmgr-gpu-value.png")
        gpu_text = _ocr(support, gpu_value, output, "app-taskmgr-gpu-value", psm=7)
        gpu_unavailable = _normalized(support, gpu_text, "N/A")
        gpu_percent = re.fullmatch(r"\s*(\d{1,3})\s*%\s*", gpu_text)
        if not (gpu_unavailable or (gpu_percent and int(gpu_percent.group(1)) <= 100)):
            raise AssertionError(f"GPU current value was invalid: {gpu_text!r}")
        perf_labels = {
            **perf_fields["labels"],
            "gpu": _normalized(support, gpu_title_text, "GPU"),
        }
        perf_value_checks = {
            **perf_fields["value_checks"],
            "gpu_value": bool(gpu_unavailable or
                              (gpu_percent and int(gpu_percent.group(1)) <= 100)),
        }
        if not all(perf_labels.values()):
            raise AssertionError(
                f"performance labels were missing in aligned crops: {perf_labels}; "
                f"fields={perf_fields['texts']!r}; GPU={gpu_title_text!r}")
        if not all(perf_value_checks.values()):
            raise AssertionError(
                f"performance readings were missing or invalid: {perf_value_checks}; "
                f"values={perf_fields['values']!r}; fields={perf_fields['texts']!r}")
        checks["performance_value_checks"] = perf_value_checks
        checks["performance_labels"] = perf_labels
        checks["performance_values"] = perf_fields["values"]
        checks["performance_reading_text"] = perf_fields["texts"]
        checks["gpu_label_visible"] = True
        checks["gpu_unavailable"] = gpu_unavailable
        checks["gpu_current_value"] = gpu_text.strip()

        time.sleep(0.8)
        perf_after_frame, _, _, perf_after_path = _capture_page(
            probe, support, geometry, output, "app-taskmgr-performance-after-timer", psm=11)
        screenshots["performance_after_timer"] = perf_after_path
        perf_after_fields = _performance_fields(
            support, perf_after_frame, geometry, output,
            "app-taskmgr-performance-after-timer-fields")
        screenshots.update({f"performance_after_timer_{name}": path
                            for name, path in perf_after_fields["screenshots"].items()})
        gpu_after_x, gpu_after_width, gpu_after_y = _gpu_card_geometry(width)
        gpu_after_title = support.client_crop(
            perf_after_frame, x + gpu_after_x, y + gpu_after_y,
            max(1, gpu_after_width - 80), 24)
        gpu_after_title_path = output / "app-taskmgr-gpu-title-after-timer.png"
        gpu_after_title.save(gpu_after_title_path)
        gpu_after_title_text = _ocr(
            support, gpu_after_title, output,
            "app-taskmgr-gpu-title-after-timer", psm=7)
        gpu_after_value = support.client_crop(
            perf_after_frame, x + gpu_after_x + gpu_after_width - 68,
            y + gpu_after_y, 68, 24)
        gpu_after_value_path = output / "app-taskmgr-gpu-value-after-timer.png"
        gpu_after_value.save(gpu_after_value_path)
        gpu_after_text = _ocr(
            support, gpu_after_value, output,
            "app-taskmgr-gpu-value-after-timer", psm=7)
        gpu_after_unavailable = _normalized(support, gpu_after_text, "N/A")
        gpu_after_percent = re.fullmatch(r"\s*(\d{1,3})\s*%\s*", gpu_after_text)
        gpu_after_valid = (
            _normalized(support, gpu_after_title_text, "GPU") and
            (gpu_after_unavailable or
             (gpu_after_percent and int(gpu_after_percent.group(1)) <= 100)))
        after_value_checks = {
            **perf_after_fields["value_checks"],
            "gpu_value": bool(gpu_after_unavailable or
                              (gpu_after_percent and int(gpu_after_percent.group(1)) <= 100)),
        }
        after_labels = {
            **perf_after_fields["labels"],
            "gpu": _normalized(support, gpu_after_title_text, "GPU"),
        }
        if not all(after_labels.values()) or not all(after_value_checks.values()):
            raise AssertionError(
                f"performance data was not valid after timer tick: labels={after_labels}; "
                f"values={after_value_checks}; fields={perf_after_fields['texts']!r}; "
                f"GPU={gpu_after_title_text!r}/{gpu_after_text!r}")
        if not gpu_after_valid:
            raise AssertionError(
                f"GPU title/value was not valid after timer tick: "
                f"title={gpu_after_title_text!r}, value={gpu_after_text!r}")
        screenshots["gpu_title_after_timer"] = str(gpu_after_title_path)
        screenshots["gpu_value_after_timer"] = str(gpu_after_value_path)
        checks["performance_after_timer_valid"] = True
        checks["performance_after_timer_labels"] = after_labels
        checks["performance_after_timer_value_checks"] = after_value_checks
        checks["performance_after_timer_values"] = perf_after_fields["values"]
        checks["gpu_after_timer_current_value"] = gpu_after_text.strip()
        checks["performance_frame_distance_after_timer"] = round(
            support.frame_distance(perf_frame, perf_after_frame), 6)

        # The startup/service-manager tab is intentionally read-only here:
        # existing entries belong to the logged-in user and are never toggled
        # or removed by this acceptance test.
        probe.click(x + 180, y + 47)
        time.sleep(0.3)
        startup_frame, _, _, startup_path = _capture_page(
            probe, support, geometry, output, "app-taskmgr-startup", psm=11)
        screenshots["startup"] = startup_path
        user_crop = support.client_crop(
            startup_frame, x + 20, y + _STARTUP_USER_TOP,
            max(1, width - 40), 28)
        user_crop_path = output / "app-taskmgr-startup-user.png"
        user_crop.save(user_crop_path)
        user_text = "\n".join(
            _ocr(support, support.client_crop(startup_frame, x + field_x, y + 70,
                                              field_width, 20), output,
                 f"app-taskmgr-startup-user-{field_x}", psm=7)
            for field_x, field_width in ((23, 42), (220, 120)))
        screenshots["startup_user"] = str(user_crop_path)
        startup_list_crop = support.client_crop(
            startup_frame, x + 20, y + _STARTUP_LIST_TOP,
            max(1, width - 40), max(1, height - _STARTUP_LIST_TOP - 48))
        startup_list_path = output / "app-taskmgr-startup-list.png"
        startup_list_crop.save(startup_list_path)
        screenshots["startup_list"] = str(startup_list_path)
        list_colors = startup_list_crop.convert("RGB").getcolors(
            maxcolors=startup_list_crop.width * startup_list_crop.height)
        startup_regions = _startup_regions(width)
        startup_control_text: dict[str, str] = {}
        for name in ("enable", "remove", "tab_strip"):
            region = support.client_crop(
                startup_frame, x + startup_regions[name][0],
                y + startup_regions[name][1], startup_regions[name][2],
                startup_regions[name][3])
            tag = f"app-taskmgr-startup-{name.replace('_', '-')}-label"
            path = output / f"{tag}.png"
            region.save(path)
            screenshots[f"startup_{name}"] = str(path)
            startup_control_text[name] = _ocr(
                support, region, output, tag, psm=6 if name == "tab_strip" else 7,
                scale=3 if name == "remove" else 2)
        startup_labels = {
            "user": _normalized(support, user_text, "用户"),
            "test_user": _normalized(support, user_text, "test"),
            "startup_entries": _normalized(
                support, startup_control_text["tab_strip"], "启动应用程序"),
            "enable": _normalized(support, startup_control_text["enable"], "启用"),
            "remove": _normalized(support, startup_control_text["remove"], "删除"),
            "entry_list_visible": bool(list_colors and len(list_colors) > 1),
        }
        if not all(startup_labels.values()):
            raise AssertionError(
                f"startup page controls/user were not visible: {startup_labels}; "
                f"user={user_text!r}; controls={startup_control_text!r}")
        checks["startup_page_labels"] = startup_labels
        checks["startup_control_text"] = startup_control_text
        checks["startup_page_read_only"] = True
        checks["startup_mutation_tested"] = False
        checks["startup_mutation_gap"] = (
            "Enable/disable/remove backend calls were preserved and source-reviewed; "
            "GUI mutations are not exercised against existing startup entries.")

        evidence["passed"] = True
    except Exception as error:
        evidence["error"] = f"{type(error).__name__}: {error}"
        failures.append(f"taskmgr: GUI regression failed: {evidence['error']}")
        if modal_may_be_open:
            try:
                probe.key("esc")
            except Exception:
                pass
    return evidence
