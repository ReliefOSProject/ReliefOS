#!/usr/bin/env python3
"""Export editor commands from completed Kbuild .cmd records; never guess flags."""
from __future__ import annotations

import argparse
import json
import re
import shlex
from pathlib import Path

REGIONS = ("kernel", "libc", "userland", "host", "all")
COMMAND = re.compile(r"^savedcmd_(.+\.o) := (.*)$", re.MULTILINE)
SOURCE = re.compile(r"^source_[^\n]* := (.*)$", re.MULTILINE)


def source_region(root: Path, source: Path) -> str | None:
    try:
        relative = source.relative_to(root).as_posix()
    except ValueError:
        return None
    if relative.startswith("kernel/reliefnt/") and "/out/" not in relative:
        if relative.startswith("kernel/reliefnt/scripts/"):
            return None
        if relative.startswith(("kernel/reliefnt/tools/host/", "kernel/reliefnt/tests/host/", "kernel/reliefnt/tools/tests/")):
            return "host"
        return "kernel" if "/third_party/" not in relative else None
    if relative.startswith(("userland/runtime/", "userland/auth/")):
        return "libc"
    if relative.startswith("userland/"):
        return "userland"
    if relative.startswith("tools/host/"):
        return "host"
    return None


def build_directory(record: Path, target: str, fallback: Path) -> Path:
    """Recover each native sub-build's cwd from its object and recorded target."""
    output = record.with_name(record.name[1:-4])
    target_path = Path(target)
    if not target_path.is_absolute():
        directory = output
        for _ in target_path.parts:
            directory = directory.parent
        if (directory / target_path).resolve() == output.resolve():
            return directory
    for directory in record.parents:
        if (directory / ".reliefos-out").is_file():
            return directory
    return fallback


def entries(root: Path, region: str, build: Path) -> list[dict[str, str | list[str]]]:
    found = []
    for record in sorted(build.rglob(".*.o.cmd")):
        data = record.read_text(encoding="utf-8")
        command, source = COMMAND.search(data), SOURCE.search(data)
        if command is None or source is None:
            continue
        directory = build_directory(record, command[1], build)
        path = (directory / source[1]).resolve()
        kind = source_region(root, path)
        if kind is None or (region != "all" and kind != region) or not path.is_file():
            continue
        # Kbuild quotes these characters for Make when saving the shell command.
        argv = shlex.split(command[2].replace(r"\#", "#").replace("$(pound)", "#").replace("$$", "$"))
        if ";" in argv:
            argv = argv[:argv.index(";")]
        if "-c" not in argv:
            continue
        found.append({"directory": str(directory), "file": str(path), "arguments": argv})
    return found


def generate(root: Path, region: str, output: Path, build: Path | None = None) -> int:
    root = root.resolve()
    build = (build or root / "out/x86_64/release").resolve()
    commands = entries(root, region, build)
    if not commands:
        raise SystemExit(f"no built Kbuild commands for {region} in {build}; build that output first or pass --build-dir")
    output = output if output.is_absolute() else root / output
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(commands, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {len(commands)} actual Kbuild commands to {output}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--region", choices=REGIONS, default="all")
    parser.add_argument("--output", type=Path, default=Path("out/vscode/compile_commands.json"))
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--build-dir", type=Path, help="completed O directory; defaults to out/x86_64/release")
    args = parser.parse_args()
    return generate(args.root, args.region, args.output, args.build_dir)


if __name__ == "__main__":
    raise SystemExit(main())
