#!/usr/bin/env python3
from __future__ import annotations

import argparse
import struct
from pathlib import Path


SIZE = 16
BMP_SIZE = 32
FONT3 = {
    "A": ["111", "101", "111", "101", "101"],
    "C": ["111", "100", "100", "100", "111"],
    "D": ["110", "101", "101", "101", "110"],
    "E": ["111", "100", "111", "100", "111"],
    "G": ["111", "100", "101", "101", "111"],
    "I": ["111", "010", "010", "010", "111"],
    "M": ["101", "111", "111", "101", "101"],
    "O": ["111", "101", "101", "101", "111"],
    "S": ["111", "100", "111", "001", "111"],
    "T": ["111", "010", "010", "010", "010"],
    "U": ["101", "101", "101", "101", "111"],
    "$": ["111", "110", "111", "011", "111"],
    "!": ["010", "010", "010", "000", "010"],
}


def rgba(r: int, g: int, b: int, a: int = 255) -> tuple[int, int, int, int]:
    return (r, g, b, a)


TRANSPARENT = rgba(0, 0, 0, 0)
BLACK = rgba(24, 24, 24)
WHITE = rgba(255, 255, 255)
DARK = rgba(68, 68, 76)
MID = rgba(128, 132, 140)
LIGHT = rgba(214, 218, 224)
BLUE = rgba(42, 104, 198)
CYAN = rgba(36, 170, 190)
GREEN = rgba(48, 166, 92)
YELLOW = rgba(246, 196, 65)
ORANGE = rgba(224, 122, 54)
RED = rgba(210, 70, 70)
PURPLE = rgba(126, 92, 180)


def blank() -> list[list[tuple[int, int, int, int]]]:
    return [[TRANSPARENT for _ in range(SIZE)] for _ in range(SIZE)]


def put(c: list[list[tuple[int, int, int, int]]], x: int, y: int,
        color: tuple[int, int, int, int]) -> None:
    if 0 <= x < SIZE and 0 <= y < SIZE:
        c[y][x] = color


def fill(c: list[list[tuple[int, int, int, int]]], x: int, y: int,
         w: int, h: int, color: tuple[int, int, int, int]) -> None:
    for yy in range(y, y + h):
        for xx in range(x, x + w):
            put(c, xx, yy, color)


def rect(c: list[list[tuple[int, int, int, int]]], x: int, y: int,
         w: int, h: int, color: tuple[int, int, int, int]) -> None:
    fill(c, x, y, w, 1, color)
    fill(c, x, y + h - 1, w, 1, color)
    fill(c, x, y, 1, h, color)
    fill(c, x + w - 1, y, 1, h, color)


def text3(c: list[list[tuple[int, int, int, int]]], x: int, y: int,
          text: str, color: tuple[int, int, int, int]) -> None:
    cx = x
    for ch in text:
        glyph = FONT3.get(ch)
        if not glyph:
            cx += 4
            continue
        for yy, row in enumerate(glyph):
            for xx, bit in enumerate(row):
                if bit == "1":
                    put(c, cx + xx, y + yy, color)
        cx += 4


def line(c: list[list[tuple[int, int, int, int]]], x0: int, y0: int,
         x1: int, y1: int, color: tuple[int, int, int, int]) -> None:
    dx = abs(x1 - x0)
    sx = 1 if x0 < x1 else -1
    dy = -abs(y1 - y0)
    sy = 1 if y0 < y1 else -1
    err = dx + dy
    while True:
        put(c, x0, y0, color)
        if x0 == x1 and y0 == y1:
            break
        e2 = 2 * err
        if e2 >= dy:
            err += dy
            x0 += sx
        if e2 <= dx:
            err += dx
            y0 += sy


def draw_document(c: list[list[tuple[int, int, int, int]]],
                  accent: tuple[int, int, int, int]) -> None:
    fill(c, 4, 2, 9, 12, WHITE)
    rect(c, 4, 2, 9, 12, DARK)
    fill(c, 10, 2, 3, 3, LIGHT)
    line(c, 10, 2, 12, 4, MID)
    fill(c, 6, 6, 5, 1, accent)
    fill(c, 6, 8, 5, 1, MID)
    fill(c, 6, 10, 4, 1, MID)


def icon_fileman(c: list[list[tuple[int, int, int, int]]]) -> None:
    fill(c, 2, 5, 12, 8, YELLOW)
    fill(c, 3, 3, 5, 3, rgba(255, 224, 128))
    fill(c, 2, 6, 12, 1, rgba(255, 238, 150))
    rect(c, 2, 5, 12, 8, rgba(150, 104, 28))


def icon_terminal(c: list[list[tuple[int, int, int, int]]]) -> None:
    fill(c, 2, 3, 12, 10, BLACK)
    rect(c, 2, 3, 12, 10, MID)
    line(c, 4, 6, 6, 8, GREEN)
    line(c, 6, 8, 4, 10, GREEN)
    fill(c, 8, 10, 4, 1, GREEN)


def icon_taskmgr(c: list[list[tuple[int, int, int, int]]]) -> None:
    fill(c, 2, 3, 12, 10, WHITE)
    rect(c, 2, 3, 12, 10, DARK)
    fill(c, 3, 4, 10, 1, BLUE)
    fill(c, 4, 10, 2, 2, GREEN)
    fill(c, 7, 7, 2, 5, GREEN)
    fill(c, 10, 5, 2, 7, GREEN)


def icon_settings(c: list[list[tuple[int, int, int, int]]]) -> None:
    fill(c, 7, 2, 2, 12, MID)
    fill(c, 2, 7, 12, 2, MID)
    fill(c, 4, 4, 8, 8, LIGHT)
    rect(c, 4, 4, 8, 8, DARK)
    fill(c, 6, 6, 4, 4, BLUE)
    fill(c, 7, 7, 2, 2, WHITE)


def icon_calc(c: list[list[tuple[int, int, int, int]]]) -> None:
    fill(c, 3, 2, 10, 12, MID)
    rect(c, 3, 2, 10, 12, DARK)
    fill(c, 4, 3, 8, 3, rgba(198, 232, 240))
    for yy in range(2):
        for xx in range(3):
            fill(c, 4 + xx * 3, 8 + yy * 3, 2, 2, WHITE)


def icon_minesweeper(c: list[list[tuple[int, int, int, int]]]) -> None:
    fill(c, 6, 3, 4, 10, BLACK)
    fill(c, 3, 6, 10, 4, BLACK)
    fill(c, 4, 4, 8, 8, BLACK)
    put(c, 6, 6, WHITE)
    put(c, 11, 4, RED)


def icon_glxgears(c: list[list[tuple[int, int, int, int]]]) -> None:
    """Small three-gear mark for the PortableGL renderer demo."""
    for cx, cy, color in ((5, 6, RED), (10, 6, GREEN), (8, 11, BLUE)):
        fill(c, cx - 2, cy - 1, 5, 3, color)
        fill(c, cx - 1, cy - 2, 3, 5, color)
        put(c, cx, cy, WHITE)


def icon_run(c: list[list[tuple[int, int, int, int]]]) -> None:
    fill(c, 3, 4, 10, 8, BLUE)
    rect(c, 3, 4, 10, 8, DARK)
    line(c, 6, 6, 6, 10, WHITE)
    line(c, 7, 7, 9, 8, WHITE)
    line(c, 9, 8, 7, 9, WHITE)


def icon_desktop(c: list[list[tuple[int, int, int, int]]]) -> None:
    fill(c, 2, 3, 12, 8, CYAN)
    rect(c, 2, 3, 12, 8, DARK)
    fill(c, 5, 12, 6, 1, MID)
    fill(c, 4, 13, 8, 1, DARK)


def icon_hello(c: list[list[tuple[int, int, int, int]]]) -> None:
    fill(c, 3, 4, 10, 7, WHITE)
    rect(c, 3, 4, 10, 7, BLUE)
    put(c, 5, 11, WHITE)
    put(c, 6, 12, BLUE)
    put(c, 6, 7, BLUE)
    put(c, 9, 7, BLUE)
    fill(c, 6, 9, 4, 1, GREEN)


def icon_uidemo(c: list[list[tuple[int, int, int, int]]]) -> None:
    fill(c, 2, 3, 12, 10, WHITE)
    rect(c, 2, 3, 12, 10, DARK)
    fill(c, 3, 4, 10, 2, BLUE)
    fill(c, 4, 8, 3, 2, GREEN)
    fill(c, 9, 8, 3, 2, ORANGE)
    fill(c, 4, 11, 8, 1, MID)


def icon_cjktest(c: list[list[tuple[int, int, int, int]]]) -> None:
    draw_document(c, RED)
    rect(c, 6, 6, 5, 5, RED)
    line(c, 8, 6, 8, 10, RED)
    line(c, 6, 8, 10, 8, RED)


def icon_shell(c: list[list[tuple[int, int, int, int]]]) -> None:
    icon_terminal(c)
    text3(c, 4, 5, "$", rgba(130, 255, 130))
    fill(c, 9, 10, 3, 1, CYAN)


def icon_osver(c: list[list[tuple[int, int, int, int]]]) -> None:
    fill(c, 4, 3, 8, 10, BLUE)
    rect(c, 4, 3, 8, 10, DARK)
    text3(c, 6, 6, "I", WHITE)


def icon_memtest(c: list[list[tuple[int, int, int, int]]]) -> None:
    fill(c, 3, 4, 10, 8, GREEN)
    rect(c, 3, 4, 10, 8, DARK)
    for x in range(4, 13, 2):
        put(c, x, 3, MID)
        put(c, x, 12, MID)
    fill(c, 5, 6, 6, 4, rgba(120, 220, 140))


def icon_bugtest(c: list[list[tuple[int, int, int, int]]]) -> None:
    line(c, 8, 2, 2, 13, YELLOW)
    line(c, 8, 2, 14, 13, YELLOW)
    line(c, 2, 13, 14, 13, YELLOW)
    fill(c, 6, 12, 5, 1, YELLOW)
    text3(c, 7, 7, "!", BLACK)


def icon_netctl(c: list[list[tuple[int, int, int, int]]]) -> None:
    fill(c, 2, 3, 12, 10, WHITE)
    rect(c, 2, 3, 12, 10, DARK)
    fill(c, 3, 4, 10, 2, BLUE)
    fill(c, 4, 8, 2, 2, GREEN)
    fill(c, 10, 8, 2, 2, GREEN)
    line(c, 6, 9, 8, 7, CYAN)
    line(c, 8, 7, 10, 9, CYAN)
    put(c, 8, 11, ORANGE)
    put(c, 7, 12, ORANGE)
    put(c, 9, 12, ORANGE)


def icon_servicemgr(c: list[list[tuple[int, int, int, int]]]) -> None:
    fill(c, 3, 2, 10, 12, WHITE)
    rect(c, 3, 2, 10, 12, DARK)
    for y in (4, 7, 10):
        rect(c, 5, y, 2, 2, BLUE)
        fill(c, 8, y, 3, 1, DARK)
    line(c, 5, 5, 6, 6, GREEN)
    line(c, 6, 6, 8, 4, GREEN)
    put(c, 11, 11, ORANGE)


def icon_httpget(c: list[list[tuple[int, int, int, int]]]) -> None:
    fill(c, 2, 4, 12, 8, WHITE)
    rect(c, 2, 4, 12, 8, DARK)
    fill(c, 3, 5, 10, 2, CYAN)
    text3(c, 4, 8, "GET", BLUE)
    put(c, 12, 11, GREEN)


def icon_downloadmgr(c: list[list[tuple[int, int, int, int]]]) -> None:
    fill(c, 3, 10, 10, 3, LIGHT)
    rect(c, 3, 10, 10, 3, DARK)
    fill(c, 7, 2, 2, 7, BLUE)
    line(c, 4, 7, 8, 11, BLUE)
    line(c, 12, 7, 8, 11, BLUE)
    fill(c, 5, 13, 6, 1, GREEN)


def icon_browser(c: list[list[tuple[int, int, int, int]]]) -> None:
    fill(c, 2, 3, 12, 10, WHITE)
    rect(c, 2, 3, 12, 10, DARK)
    fill(c, 3, 4, 10, 2, BLUE)
    fill(c, 4, 8, 8, 1, CYAN)
    fill(c, 4, 10, 5, 1, CYAN)
    line(c, 10, 9, 13, 12, ORANGE)
    line(c, 13, 12, 12, 13, ORANGE)
    put(c, 11, 10, ORANGE)


def icon_imageview(c: list[list[tuple[int, int, int, int]]]) -> None:
    fill(c, 2, 3, 12, 10, WHITE)
    rect(c, 2, 3, 12, 10, DARK)
    fill(c, 3, 4, 10, 8, rgba(204, 232, 246))
    fill(c, 4, 10, 8, 2, GREEN)
    line(c, 4, 11, 7, 8, GREEN)
    line(c, 7, 8, 10, 11, GREEN)
    fill(c, 10, 5, 2, 2, YELLOW)


def icon_paint(c: list[list[tuple[int, int, int, int]]]) -> None:
    """Paint palette and brush mark for the drawing application."""
    fill(c, 2, 9, 10, 4, WHITE)
    rect(c, 2, 9, 10, 4, DARK)
    for x, swatch in ((4, RED), (6, YELLOW), (8, GREEN), (10, BLUE)):
        put(c, x, 10, swatch)
    line(c, 8, 7, 13, 2, ORANGE)
    line(c, 9, 8, 14, 3, DARK)
    fill(c, 12, 2, 3, 2, ORANGE)
    put(c, 8, 7, BLUE)


def icon_wavplay(c: list[list[tuple[int, int, int, int]]]) -> None:
    fill(c, 3, 6, 4, 5, BLUE)
    fill(c, 6, 4, 3, 9, BLUE)
    line(c, 9, 6, 12, 4, CYAN)
    line(c, 9, 10, 12, 12, CYAN)
    put(c, 4, 12, DARK)
    put(c, 11, 7, GREEN)
    put(c, 12, 8, GREEN)


def icon_diskmgr(c: list[list[tuple[int, int, int, int]]]) -> None:
    fill(c, 3, 4, 10, 2, LIGHT)
    rect(c, 3, 4, 10, 2, DARK)
    fill(c, 3, 7, 10, 2, rgba(190, 200, 210))
    rect(c, 3, 7, 10, 2, DARK)
    fill(c, 3, 10, 10, 2, MID)
    rect(c, 3, 10, 10, 2, DARK)
    put(c, 11, 4, GREEN)
    put(c, 11, 7, GREEN)
    put(c, 11, 10, GREEN)


def icon_devmgr(c: list[list[tuple[int, int, int, int]]]) -> None:
    fill(c, 4, 4, 8, 8, PURPLE)
    rect(c, 4, 4, 8, 8, DARK)
    for p in range(5, 12, 2):
        put(c, p, 3, MID)
        put(c, p, 12, MID)
        put(c, 3, p, MID)
        put(c, 12, p, MID)
    fill(c, 6, 6, 4, 4, rgba(190, 170, 230))


def icon_drvmgr(c: list[list[tuple[int, int, int, int]]]) -> None:
    icon_devmgr(c)
    fill(c, 6, 6, 4, 4, BLUE)
    text3(c, 6, 6, "D", WHITE)


def icon_oobe(c: list[list[tuple[int, int, int, int]]]) -> None:
    fill(c, 4, 3, 8, 10, WHITE)
    rect(c, 4, 3, 8, 10, DARK)
    put(c, 6, 6, GREEN)
    line(c, 7, 7, 10, 5, GREEN)
    put(c, 6, 10, GREEN)
    line(c, 7, 11, 10, 9, GREEN)


def icon_login(c: list[list[tuple[int, int, int, int]]]) -> None:
    rect(c, 5, 3, 6, 6, BLUE)
    fill(c, 4, 8, 8, 6, BLUE)
    rect(c, 4, 8, 8, 6, DARK)
    put(c, 8, 10, WHITE)
    fill(c, 8, 11, 1, 2, WHITE)


def icon_installer(c: list[list[tuple[int, int, int, int]]]) -> None:
    fill(c, 3, 7, 10, 6, ORANGE)
    rect(c, 3, 7, 10, 6, DARK)
    line(c, 8, 2, 8, 8, BLUE)
    line(c, 5, 6, 8, 9, BLUE)
    line(c, 11, 6, 8, 9, BLUE)


def draw_icon(name: str) -> list[list[tuple[int, int, int, int]]]:
    c = blank()
    if name == "fileman":
        icon_fileman(c)
    elif name == "terminal":
        icon_terminal(c)
    elif name == "notepad":
        draw_document(c, BLUE)
    elif name == "settings":
        icon_settings(c)
    elif name == "calc":
        icon_calc(c)
    elif name == "minesweeper":
        icon_minesweeper(c)
    elif name == "glxgears":
        icon_glxgears(c)
    elif name == "taskmgr":
        icon_taskmgr(c)
    elif name == "run":
        icon_run(c)
    elif name == "desktop":
        icon_desktop(c)
    elif name == "init":
        icon_oobe(c)
        put(c, 12, 2, GREEN)
    elif name == "hello":
        icon_hello(c)
    elif name == "uidemo":
        icon_uidemo(c)
    elif name == "cjktest":
        icon_cjktest(c)
    elif name == "shell":
        icon_shell(c)
    elif name == "osver":
        icon_osver(c)
    elif name == "memtest":
        icon_memtest(c)
    elif name == "bugtest":
        icon_bugtest(c)
    elif name == "netctl":
        icon_netctl(c)
    elif name in {"sessiond", "device-agent"}:
        icon_servicemgr(c)
    elif name == "servicemgr":
        icon_servicemgr(c)
    elif name == "httpget":
        icon_httpget(c)
    elif name == "downloadmgr":
        icon_downloadmgr(c)
    elif name == "browser":
        icon_browser(c)
    elif name == "imageview":
        icon_imageview(c)
    elif name == "paint":
        icon_paint(c)
    elif name == "wavplay":
        icon_wavplay(c)
    elif name == "oshlp":
        draw_document(c, CYAN)
        text3(c, 6, 6, "!", BLUE)
    elif name == "diskmgr":
        icon_diskmgr(c)
    elif name == "devmgr":
        icon_devmgr(c)
    elif name == "drvmgr":
        icon_drvmgr(c)
    elif name == "oobe":
        icon_oobe(c)
    elif name == "login":
        icon_login(c)
    elif name == "installer":
        icon_installer(c)
    else:
        draw_document(c, PURPLE)
    return c


def write_bmp(path: Path, pixels: list[list[tuple[int, int, int, int]]]) -> None:
    image_size = BMP_SIZE * BMP_SIZE * 4
    pixel_offset = 14 + 40
    file_size = pixel_offset + image_size
    header = struct.pack("<2sIHHI", b"BM", file_size, 0, 0, pixel_offset)
    dib = struct.pack("<IiiHHIIiiII", 40, BMP_SIZE, -BMP_SIZE, 1, 32, 0,
                      image_size, 2835, 2835, 0, 0)
    data = bytearray()
    for y in range(BMP_SIZE):
        src_y = y * SIZE // BMP_SIZE
        for x in range(BMP_SIZE):
            src_x = x * SIZE // BMP_SIZE
            r, g, b, a = pixels[src_y][src_x]
            data.extend((b, g, r, a))
    path.write_bytes(header + dib + data)


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate ReliefOS BMP app icons")
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--apps", nargs="+", required=True)
    args = parser.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    for name in args.apps:
        write_bmp(out_dir / f"{name}.bmp", draw_icon(name))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
