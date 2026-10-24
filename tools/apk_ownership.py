#!/usr/bin/env python3
"""Inventory build-distributed files, without pretending they are installed APKs."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import posixpath
import re
import stat
import tomllib

import reliefos_layout as layout
from storage_tools import FORMATTER_COMMANDS, UTIL_LINUX_COMMANDS, UTIL_LINUX_LIBRARIES

ROOT = Path(__file__).resolve().parents[1]


def load_policy(policy_path: Path, components_path: Path):
    policy = json.loads(policy_path.read_text())
    if policy.get("version") != 1 or policy.get("apk_registration") != "not-installed":
        raise ValueError("unsupported ownership policy or invented APK registration")
    components = tomllib.loads(components_path.read_text())["components"]
    expected = {item["id"] for item in components}
    owners, rules = {}, {}

    def claim(path, group):
        if (not path or path.startswith("/") or
                any(p in ("", ".", "..") for p in path.split("/"))):
            raise ValueError(f"invalid guest path: {path!r}")
        if path in rules and rules[path] != group:
            raise ValueError(f"ambiguous ownership: {path}")
        rules[path] = group

    for group, entry in policy["groups"].items():
        if entry["destination"] not in ("reliefos", "alpine-after-validation"):
            raise ValueError(f"invalid destination: {group}")
        for component in entry["components"]:
            if component in owners:
                raise ValueError(f"duplicate component: {component}")
            owners[component] = group
            claim(str(layout.app_package_dir(component)), group)
            for size in (16, 32, 48, 64, 128, 256):
                claim(f"usr/share/icons/hicolor/{size}x{size}/apps/reliefos-{component}.png", group)
            for path in layout.tool_payload_paths(component):
                claim(str(path), group)
        for path in entry.get("paths", []):
            claim(path, group)
        for stem in entry.get("library_stems", []):
            for suffix in (".so", ".a", ".la"):
                claim(stem + suffix, group)
    if set(owners) != expected:
        raise ValueError(f"component policy mismatch: {sorted(set(owners) ^ expected)}")
    for path in (*UTIL_LINUX_COMMANDS, *UTIL_LINUX_LIBRARIES):
        claim(path, "storage-util-linux")
    for path in FORMATTER_COMMANDS:
        claim(path, "storage-filesystems")
    return policy, rules


def classify(name: str, rules):
    path = PurePosixPath(name)
    for parent in (path, *path.parents):
        if str(parent) in rules:
            return rules[str(parent)], "path-rule"
    versioned = re.fullmatch(r"(.+\.so)(?:\.[0-9]+)+", name)
    if versioned and versioned[1] in rules:
        return rules[versioned[1]], "path-rule"
    return "reliefos-base", "fallback-needs-review"


def inventory(root: Path, rules):
    if root.is_symlink() or not root.is_dir():
        raise ValueError("staging root must be a real directory")
    entries = []
    # os.walk does not follow guest symlink directories into the host.
    for parent, directories, files in os.walk(root, followlinks=False):
        for basename in sorted(directories + files):
            path = Path(parent) / basename
            info = path.lstat()
            if stat.S_ISDIR(info.st_mode):
                continue  # Shared directories are not exclusive package claims.
            name = path.relative_to(root).as_posix()
            group, basis = classify(name, rules)
            entry = {"path": "/" + name, "mode": f"{stat.S_IMODE(info.st_mode):04o}"}
            if stat.S_ISLNK(info.st_mode):
                target = os.readlink(path)
                entry.update(type="symlink", target=target)
                # Resolve lexically within guest /; never dereference host files.
                seen, current = set(), name
                while current not in seen:
                    seen.add(current)
                    link_path = root / current
                    # Do not traverse intermediate directory symlinks.
                    parents = list(reversed(link_path.parents))
                    if any(p.is_symlink() for p in parents if p != root and p.is_relative_to(root)):
                        break
                    if not link_path.is_symlink():
                        break
                    link = os.readlink(link_path)
                    current = posixpath.normpath(posixpath.join("/", posixpath.dirname(current), link)).lstrip("/")
                    candidate, candidate_basis = classify(current, rules)
                    if candidate_basis == "path-rule":
                        group, basis = candidate, "guest-symlink-target"
            elif stat.S_ISREG(info.st_mode):
                with path.open("rb") as stream:
                    digest = hashlib.file_digest(stream, "sha256").hexdigest()
                entry.update(type="file", size=info.st_size, sha256=digest)
            else:
                raise ValueError(f"unexpected staging special file: {name}")
            entry.update(group=group, classification=basis)
            entries.append(entry)
    return sorted(entries, key=lambda item: item["path"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.resolve().is_relative_to(args.root.resolve()):
        parser.error("inventory output must be outside staging (avoid a self-referential checksum)")
    policy, rules = load_policy(ROOT / "configs/apk-ownership.json", ROOT / "configs/components.toml")
    entries = inventory(args.root, rules)
    result = {"version": 1, "scope": "host-staging-snapshot-not-installed-root",
              "apk_registration": "not-installed", "policy": policy, "files": entries}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    fallback = sum(item["classification"] == "fallback-needs-review" for item in entries)
    print(f"APK preparation inventory: {len(entries)} files, {fallback} fallback entries requiring review")


if __name__ == "__main__":
    main()
