#!/usr/bin/env python3
"""Check the signed ncurses command/terminfo payload in a managed root on Linux."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--musl", required=True, type=Path)
    parser.add_argument("--root", required=True, type=Path)
    args = parser.parse_args()
    musl, root = args.musl.resolve(), args.root.resolve()
    database = root / "usr/share/terminfo"
    env = {**os.environ, "TERM": "xterm-256color", "TERMINFO": str(database),
           "TERMINFO_DIRS": str(database)}
    loader = musl / "lib/libc.so"
    def run(command, *arguments):
        return subprocess.check_output([str(loader), "--library-path", str(root / "usr/lib") + ":" + str(root / "lib"),
                                        str(root / "usr/bin" / command), *arguments], env=env)
    for command in ("clear", "infocmp", "tput"):
        headers = subprocess.check_output(["readelf", "-l", str(root / "usr/bin" / command)], text=True)
        assert "/lib/ld-musl-x86_64.so.1" in headers
    terminfo = run("infocmp", "xterm-256color")
    assert b"colors#0x100" in terminfo or b"colors#256" in terminfo
    assert run("clear").startswith(b"\x1b[")
    assert int(run("tput", "cols")) > 0
    print("PASS Linux host: signed dynamic ncurses tools and matching terminfo")


if __name__ == "__main__":
    main()
