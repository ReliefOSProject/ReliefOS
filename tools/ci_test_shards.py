#!/usr/bin/env python3
"""Partition the complete build-test inventory without dropping new suites."""
from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path
import re
import sys


OFFLOADED_TESTS = {
    "kernel": (
        "test-concurrency.sh",
        "test-incremental.sh",
        "test-kernel-adapter.sh",
        "test-header-boundary.sh",
        "test-reproducible.sh",
        "test-submodule-contract.sh",
        "test-submodule-rollback-build.sh",
    ),
    "auth": ("test-auth-stage.sh",),
    "pages": ("test-pages.sh",),
}
GROUPS = ("all", "core", "kernel", "auth", "pages")


def partition(root: Path, offloaded=None) -> dict[str, tuple[str, ...]]:
    """Validate explicit shards; unassigned present and future suites are core."""
    offloaded = OFFLOADED_TESTS if offloaded is None else offloaded
    if set(offloaded) != set(GROUPS[2:]):
        raise ValueError("Expected exactly the kernel, auth and pages test shards")
    directory = root / "tests/build"
    inventory = tuple(sorted(path.name for path in directory.glob("test-*.sh") if path.is_file()))
    if not inventory:
        raise ValueError(f"No build test suites found in {directory}")
    assigned = []
    for group, names in offloaded.items():
        if not names:
            raise ValueError(f"Empty explicit test shard: {group}")
        for name in names:
            if not isinstance(name, str) or not re.fullmatch(r"test-[A-Za-z0-9_-]+\.sh", name):
                raise ValueError(f"Invalid build suite name in {group}: {name!r}")
            if name not in inventory:
                raise ValueError(f"Missing build suite in {group}: {name}")
            assigned.append(name)
    duplicates = sorted(name for name, count in Counter(assigned).items() if count != 1)
    if duplicates:
        raise ValueError("Build suites assigned more than once: " + ", ".join(duplicates))
    result = {group: tuple(sorted(names)) for group, names in offloaded.items()}
    result["core"] = tuple(name for name in inventory if name not in assigned)
    combined = tuple(name for group in GROUPS[1:] for name in result[group])
    if len(combined) != len(inventory) or set(combined) != set(inventory):
        raise ValueError("CI test shards do not cover the complete build-test inventory")
    result["all"] = inventory
    return result


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    command = parser.add_mutually_exclusive_group(required=True)
    command.add_argument("--group", choices=GROUPS)
    command.add_argument("--check", action="store_true")
    command.add_argument("--groups-json", action="store_true",
                         help="Emit the validated focused-job matrix")
    arguments = parser.parse_args(argv)
    try:
        groups = partition(arguments.root)
    except (OSError, ValueError) as error:
        parser.exit(1, f"CI test shards: {error}\n")
    if arguments.groups_json:
        print(json.dumps(list(GROUPS[2:])))
    elif arguments.check:
        print("Validated " + ", ".join(f"{group}={len(groups[group])}" for group in GROUPS[1:])
              + f"; all {len(groups['all'])} build suites are assigned exactly once")
    else:
        print(" ".join(groups[arguments.group]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
