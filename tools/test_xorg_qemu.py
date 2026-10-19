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
    re.compile(r"icewm started for uid=(?!0\b)\d+\b"),
    re.compile(r"st started\b"),
    re.compile(r"xdm session ended\b"),
    re.compile(r"tty1 restored to text login\b"),
]


def source_check():
    errors = []
    required = {
        "docs/XORG.md": ["IceWM", "make fetch", "Dillo", "QEMU"],
        "docs/APK_PREPARATION.md": ["feature=xorg", "xdm", "v3.24"],
        "system/xorg/xdm-Xservers": ["/usr/lib/reliefos/xorg-tty-wrapper", "vt1", "-keeptty"],
        "system/xorg/xdm-session": ["/usr/bin/icewm", "/usr/bin/st",
                                    "xdg-user-dirs-update"],
        "system/xorg/icewm/menu": ["fileman", "calc", "dillo", "nedit"],
        "system/xorg/icewm/theme": ["Theme="],
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
    xorg_conf = ROOT / "system/xorg/xorg.conf"
    if xorg_conf.is_file():
        config = xorg_conf.read_text()
        device = re.search(r'(?ims)^Section\s+"Device"\s*\n(.*?)^EndSection\s*$', config)
        if not device:
            errors.append("system/xorg/xorg.conf: missing a Device section for stock Xorg auto-configuration")
        else:
            device_text = device.group(1)
            if re.search(r"(?im)^\s*Driver\s+", device_text):
                errors.append("system/xorg/xorg.conf: Device must leave driver selection to Xorg auto-configuration")
            if re.search(r'(?im)^\s*Option\s+"(?:kmsdev|fbdev)"', device_text):
                errors.append("system/xorg/xorg.conf: Device must not force a device path")
            for option in ('Option "SWcursor" "true"', 'Option "PageFlip" "false"'):
                if option not in device_text:
                    errors.append(f"system/xorg/xorg.conf: Device missing fixed-profile option {option}")
        if 'Option "IgnoreAbsoluteAxes" "false"' not in config:
            errors.append("system/xorg/xorg.conf: preserve absolute pointer resynchronization")
    lock_path = ROOT / "configs/dependencies.lock.json"
    try:
        lock = json.loads(lock_path.read_text())
        ids = {item["id"] for item in lock["dependencies"]}
    except (OSError, KeyError, json.JSONDecodeError):
        errors.append("configs/dependencies.lock.json: unreadable package lock")
    else:
        if "alpine-pcmanfm" in ids:
            errors.append("alpine-pcmanfm must stay out of the fetch lock")
        if "alpine-icewm" not in ids:
            errors.append("configs/dependencies.lock.json: missing alpine-icewm")
        if "alpine-twm" in ids:
            errors.append("alpine-twm must stay out of the fetch lock")
        if "coreutils" not in ids:
            errors.append("configs/dependencies.lock.json: missing coreutils package lock for dd")
        # gdk-pixbuf 2.44 keeps only the legacy XPM module; every other image
        # format is decoded by the glycin loaders, so GTK icon and theme
        # pixbufs need both loaders in the fetch lock.  xdg-user-dirs supplies
        # the user directories that GTK file libraries expect at session start.
        for gid in ("alpine-glycin-image-rs", "alpine-glycin-svg", "alpine-xdg-user-dirs"):
            if gid not in ids:
                errors.append(f"configs/dependencies.lock.json: missing {gid}")
        gallium = next((item for item in lock["dependencies"]
                        if item.get("id") == "alpine-mesa-dri-gallium"), None)
        expected_gallium = {
            "version": "26.1.6-r0",
            "url": "https://dl-cdn.alpinelinux.org/alpine/v3.24/main/x86_64/mesa-dri-gallium-26.1.6-r0.apk",
            "sha256": "91a8c6d8186a0f855e545cd7d7fedeb6265e90b0d74fda2538652665941d1de5",
            "feature": "xorg",
        }
        if gallium is None:
            errors.append("configs/dependencies.lock.json: missing alpine-mesa-dri-gallium")
        else:
            for field, value in expected_gallium.items():
                if gallium.get(field) != value:
                    errors.append(f"configs/dependencies.lock.json: alpine-mesa-dri-gallium {field} must be {value}")
    session_path = ROOT / "system/xorg/xdm-session"
    if session_path.is_file() and "/var/log/" in session_path.read_text():
        errors.append("xdm-session must not write root-only logs")
    if (ROOT / "system/xorg/twmrc").exists():
        errors.append("system/xorg/twmrc must not exist")
    if session_path.is_file() and re.search(r"\btwm\b", session_path.read_text()):
        errors.append("xdm-session must not reference twm")
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
    if re.search(r"icewm started for uid=0\b", text):
        errors.append("IceWM must run as a non-root user")
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
