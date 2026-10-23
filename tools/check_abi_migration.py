#!/usr/bin/env python3
"""Inventory and optionally reject ReliefOS-private ABI use.

The migration is intentionally staged: the default mode emits a deterministic
inventory so existing consumers can be migrated without breaking the image.
CI or a release build can pass --strict after the allowlist has been reduced;
strict mode rejects private hardware APIs in application source.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TEXT_SUFFIXES = {".c", ".h", ".cc", ".cpp", ".S", ".rs", ".py", ".toml", ".md", ".rst"}
EXCLUDED_PREFIXES = ("third_party/", "build/", ".git/")
# Service daemons are the server side of a migrated protocol. They may use
# the versioned wire structs that applications are no longer allowed to see.
SERVICE_APP_PREFIXES = (
    "userland/apps/imd/",
    "userland/apps/authd/",
    "userland/apps/sessiond/",
    "userland/apps/device-agent/",
)
PRIVATE_RE = re.compile(
    r"\b(?:LEONOS_[A-Z0-9_]*(?:IOCTL|PTY|INPUTM|DISK|INSTALL|AUDIO|NET|DEVICE|DRIVER|GUI)"
    r"|LEONOS_IOCTL_GPU_[A-Z0-9_]+"
    r"|leonos_(?:pty|inputm|disk|install|audio|socket|net|device|driver|mouse|gpu)_[A-Za-z0-9_]+)\b"
)
DELETED_PRIVATE_RE = re.compile(
    r"\b(?:LEONOS_GUI_IOCTL|LEONOS_AUTH_IOCTL|LEONOS_INPUTM_IOCTL|"
    r"LEONOS_STARTUP_IOCTL|LEONOS_FS_IOCTL_|LEONOS_PTY_IOCTL|"
    r"LEONOS_IOCTL_AUDIO_|LEONOS_IOCTL_NET_|LEONOS_IOCTL_DEVICE_LIST|"
    r"LEONOS_IOCTL_DRIVER_|LEONOS_IOCTL_SYSTEM_INFO|LEONOS_IOCTL_TIME_|"
    r"LEONOS_IOCTL_MACHINE_IDENTITY|LEONOS_IOCTL_PERF_INFO|"
    r"LEONOS_IOCTL_TASK_AFFINITY|LEONOS_TEXT_IOCTL|LEONOS_IOCTL_LIST_DIR|"
    r"LEONOS_SIGNAL_IOCTL|LEONOS_KERNEL_DEBUG_IOCTL)"
    r"[A-Z0-9_]*\b"
)

HARDWARE_RE = re.compile(
    r"\b(?:LEONOS_[A-Z0-9_]*(?:IOCTL|PTY|INPUTM|DISK|INSTALL|AUDIO|NET|DEVICE|DRIVER)"
    r"|LEONOS_IOCTL_GPU_[A-Z0-9_]+"
    r"|leonos_(?:pty|inputm|disk|install|audio|socket|net|device|driver|mouse|gpu)_[A-Za-z0-9_]+)\b"
)


def tracked_files() -> list[str]:
    result = subprocess.run(
        ["git", "ls-files", "-z"], cwd=ROOT, check=True,
        stdout=subprocess.PIPE,
    )
    return [item.decode("utf-8") for item in result.stdout.split(b"\0") if item]


def scan() -> dict[str, set[str]]:
    uses: dict[str, set[str]] = defaultdict(set)
    for relative in tracked_files():
        path = Path(relative)
        if path.suffix not in TEXT_SUFFIXES or relative.startswith(EXCLUDED_PREFIXES):
            continue
        try:
            text = (ROOT / relative).read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        for match in PRIVATE_RE.finditer(text):
            uses[match.group(0)].add(relative)
    return dict(uses)


def write_report(path: Path, uses: dict[str, set[str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# Generated ReliefOS private ABI inventory",
        "# Do not edit; regenerate with tools/check_abi_migration.py.",
        "",
    ]
    for symbol in sorted(uses):
        lines.append(f"{symbol}:" )
        lines.extend(f"  - {relative}" for relative in sorted(uses[symbol]))
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def deleted_private_uses() -> list[str]:
    failures: list[str] = []
    for relative in tracked_files():
        path = Path(relative)
        if path.suffix not in TEXT_SUFFIXES or relative.startswith(EXCLUDED_PREFIXES):
            continue
        if relative.startswith("Documentation/") or relative.startswith("tools/") or \
                relative.startswith("los2w/") or relative == "tools/check_abi_migration.py":
            continue
        try:
            text = (ROOT / relative).read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        for match in DELETED_PRIVATE_RE.finditer(text):
            failures.append(f"{relative}: deleted private ioctl symbol {match.group(0)}")
    return sorted(failures)


def strict_failures(uses: dict[str, set[str]]) -> list[str]:
    failures: list[str] = list(deleted_private_uses())
    for symbol, paths in uses.items():
        for relative in paths:
            # Kernel/libc transition code is allowed until the migration table
            # marks that subsystem complete. Application code is the boundary
            # that must not grow new private hardware dependencies.
            if relative.startswith(("userland/apps/", "userland/programs/")) and \
                    not relative.startswith(SERVICE_APP_PREFIXES):
                failures.append(f"{relative}: private ABI symbol {symbol}")
    return sorted(failures)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--report", type=Path)
    parser.add_argument("--strict", action="store_true")
    args = parser.parse_args()
    uses = scan()
    if args.report:
        write_report(args.report, uses)
    print(f"ABI migration inventory: {len(uses)} private symbols")
    if args.strict:
        failures = strict_failures(uses)
        if failures:
            print("strict ABI migration check failed:", file=sys.stderr)
            print("\n".join(failures), file=sys.stderr)
            return 1
        print("strict ABI migration check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
