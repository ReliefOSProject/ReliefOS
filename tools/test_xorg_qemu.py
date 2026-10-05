#!/usr/bin/env python3
"""Validate evidence emitted by an Xorg/XDM guest run."""
from pathlib import Path
import argparse
import json
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
EVENTS = [
    re.compile(r"desktop-backend=xorg\b"),
    re.compile(r"xdm started on vt1\b"),
    re.compile(r"PAM authentication accepted\b"),
    re.compile(r"twm started for uid=(?!0\b)\d+\b"),
    re.compile(r"xterm started\b"),
    re.compile(r"xdm session ended\b"),
    re.compile(r"tty1 restored to text login\b"),
]


def source_check():
    errors = []
    required = {
        "docs/XORG.md": ["CONFIG_DESKTOP_BACKEND_XORG", "make fetch", "PCManFM", "QEMU"],
        "docs/APK_PREPARATION.md": ["feature=xorg", "xdm", "v3.24"],
        "system/xorg/xdm-Xservers": ["/usr/lib/reliefos/xorg-tty-wrapper", "vt1", "-keeptty"],
        "system/xorg/xdm-session": ["/usr/bin/twm", "/usr/bin/xterm"],
        "system/xorg/twmrc": ["PCManFM", "pcmanfm --desktop-off"],
    }
    for relative, needles in required.items():
        path = ROOT / relative
        if not path.is_file():
            errors.append(f"missing contract file: {relative}")
            continue
        text = path.read_text()
        for needle in needles:
            if needle not in text:
                errors.append(f"{relative}: missing {needle}")
    lock_path = ROOT / "configs/dependencies.lock.json"
    try:
        lock = json.loads(lock_path.read_text())
        pcmanfm = next(item for item in lock["dependencies"] if item["id"] == "alpine-pcmanfm")
    except (OSError, KeyError, StopIteration, json.JSONDecodeError):
        errors.append("configs/dependencies.lock.json: missing alpine-pcmanfm package lock")
    else:
        if pcmanfm.get("feature") != "xorg":
            errors.append("alpine-pcmanfm must be restricted to feature=xorg")
        if not pcmanfm.get("url", "").startswith("https://dl-cdn.alpinelinux.org/alpine/v3.24/"):
            errors.append("alpine-pcmanfm must come from the official Alpine v3.24 repository")
    session_path = ROOT / "system/xorg/xdm-session"
    if session_path.is_file() and "/var/log/" in session_path.read_text():
        errors.append("xdm-session must not write root-only logs")
    return errors


def log_check(path):
    text = path.read_text()
    errors = []
    cursor = 0
    for event in EVENTS:
        match = event.search(text, cursor)
        if not match:
            errors.append(f"missing or out-of-order event: {event.pattern}")
            continue
        cursor = match.end()
    if re.search(r"(?i)(password|passwd|cookie|authorization)\s*[:=]", text):
        errors.append("log contains password or authorization material")
    if re.search(r"twm started for uid=0\b", text):
        errors.append("TWM must run as a non-root user")
    return errors


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-only", action="store_true")
    parser.add_argument("--log", type=Path)
    parser.add_argument("--image", type=Path)
    args = parser.parse_args()
    errors = source_check()
    if args.log:
        if not args.log.is_file():
            errors.append(f"QEMU log does not exist: {args.log}")
        else:
            errors.extend(log_check(args.log))
    if args.image and not args.image.is_file():
        errors.append(f"QEMU image does not exist: {args.image}")
    if not args.source_only and not args.log:
        errors.append("a QEMU evidence log is required unless --source-only is used")
    if errors:
        print("xorg-qemu contract: FAIL")
        for error in errors:
            print(f"  {error}")
        return 1
    print("xorg-qemu contract: PASS")
    if args.log:
        print(f"  evidence: {args.log}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
