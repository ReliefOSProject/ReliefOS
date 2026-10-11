#!/usr/bin/env python3
"""Generate VS Code clangd/CPPTools compile commands for ReliefOS regions.

The production build is the root Makefile. This file only mirrors its freestanding
compiler model so VS Code can resolve generated headers, libc and kernel APIs.
Run it from Linux or WSL.
"""

from __future__ import annotations

import argparse
import json
import shlex
from pathlib import Path


REGION_PATTERNS: dict[str, tuple[str, ...]] = {
    "kernel": (
        "kernel/reliefnt/kernel/reliefnt/**/*.c", "kernel/reliefnt/kernel/reliefnt/**/*.S",
        "kernel/reliefnt/kernel/exec/**/*.c", "kernel/reliefnt/drivers/console/**/*.c",
        "kernel/reliefnt/drivers/bootstrap/**/*.c",
        "kernel/reliefnt/drivers/bootstrap/**/*.S",
        "kernel/reliefnt/arch/**/*.c", "kernel/reliefnt/arch/**/*.S",
        "kernel/reliefnt/mm/**/*.c", "kernel/reliefnt/fs/**/*.c", "kernel/reliefnt/net/**/*.c",
    ),
    "libc": (
        "userland/runtime/src/**/*.c", "userland/runtime/src/**/*.S",
        "userland/runtime/src/**/*.cpp",
    ),
    "userland": (
        "userland/**/*.c", "userland/**/*.cpp", "userland/**/*.cc",
        "userland/**/*.cxx", "userland/**/*.S",
    ),
}


def all_sources(root: Path, region: str) -> list[Path]:
    selected = tuple(REGION_PATTERNS) if region == "all" else (region,)
    found: set[Path] = set()
    for name in selected:
        for pattern in REGION_PATTERNS[name]:
            found.update(path for path in root.glob(pattern) if path.is_file())
    return sorted(found)


def include_flags(root: Path, region: str) -> list[str]:
    common = [root / "include", root / "build/include", root / "build/include/generated"]
    if region == "kernel":
        # Mirrors the kernel build's include order (paths relative to the
        # ReliefNT submodule root): O_INCLUDE, core private include, UAPI,
        # reliefos headers; the parent's include stays as a fallback for runtime
        # forwarders.
        paths = [root / "build/include", root / "kernel/reliefnt/kernel/reliefnt/include",
                 root / "kernel/reliefnt/include/uapi", root / "kernel/reliefnt/include",
                 root / "include", root / "build/include/generated",
                 root / "kernel/reliefnt/drivers/bootstrap"]
    elif region in {"libc", "userland"}:
        paths = common + [
            root / "userland/runtime/include",
            root / "build/musl/sysroot/include",
            root / "third_party/mbedtls/include",
            root / "third_party/zlib",
            root / "third_party/libpng",
            root / "build/generated/libpng",
            root / "userland/stardustui/include",
            root / "third_party/stardustui",
        ]
    else:
        paths = [root / "include",
                 root / "build/include", root / "userland/runtime/include",
                 root / "build/musl/sysroot/include"]
    return [f"-I{path.relative_to(root).as_posix()}" for path in paths if path.is_dir()]


def relative_path(root: Path, path: Path) -> str:
    """Return a portable path for compile_commands.json.

    Relative entries let cpptools match files opened through a Windows
    junction while clang-tidy still resolves them from the database directory.
    """
    return path.relative_to(root).as_posix()


def compiler_flags(root: Path, region: str, source: Path) -> list[str]:
    is_cpp = source.suffix in {".cpp", ".cc", ".cxx"}
    is_asm = source.suffix == ".S"
    compiler = "clang++" if is_cpp else "clang"
    flags = [compiler, "-target", "x86_64-unknown-none", "-ffreestanding"]
    generated_autoconf = root / "build/include/generated/autoconf.h"
    if is_cpp:
        flags += ["-std=c++17", "-fno-exceptions", "-fno-rtti", "-fno-use-cxa-atexit", "-nostdinc++"]
    elif not is_asm:
        # ReliefOS C sources intentionally use GNU extensions such as __asm__.
        flags += ["-std=gnu11"]
    flags += ["-fno-stack-protector", "-mno-red-zone", "-mgeneral-regs-only", "-Wall", "-Wextra"]
    if is_asm:
        flags += ["-x", "assembler-with-cpp"]
    if region == "kernel":
        flags += ["-fno-pic", "-fno-pie", "-mcmodel=kernel", "-DRELIEFOS_KERNEL=1",
                  *include_flags(root, region)]
        if generated_autoconf.is_file():
            flags += ["-include", relative_path(root, generated_autoconf)]
    elif region in {"libc", "userland"}:
        flags += ["-fPIC", *(["-fPIE"] if region == "userland" else []),
                  "-ffunction-sections", "-fdata-sections",
                  "-DRELIEFOS_USE_MUSL", "-D_POSIX_C_SOURCE=200809L",
                  '-DMBEDTLS_CONFIG_FILE="reliefos_mbedtls_config.h"',
                  f"-DRELIEFOS_{region.upper()}=1",
                  *include_flags(root, region),
                  ]
        if generated_autoconf.is_file():
            flags += ["-include", relative_path(root, generated_autoconf)]
    else:
        flags += ["-fPIC", "-fPIE", "-DRELIEFOS_USE_MUSL", "-D_POSIX_C_SOURCE=200809L",
                  "-D_DEFAULT_SOURCE", *include_flags(root, region)]
    return flags


def output_path(root: Path, source: Path) -> Path:
    relative = source.relative_to(root)
    return Path("build/vscode/objects") / relative.with_suffix(relative.suffix + ".o")


def source_region(root: Path, source: Path, selected: str) -> str:
    # A source can match multiple broad patterns (notably userland/runtime).
    # Keep the most specific region first so its flags and headers win.
    if source.is_relative_to(root / "userland/runtime") or source.is_relative_to(root / "third_party/mbedtls"):
        return "libc"
    if source.is_relative_to(root / "kernel"):
        return "kernel"
    return "userland" if selected == "all" else selected


def generate(root: Path, region: str, output: Path) -> int:
    root = root.absolute()
    output = output if output.is_absolute() else root / output
    output.parent.mkdir(parents=True, exist_ok=True)
    entries = []
    for source in all_sources(root, region):
        command = compiler_flags(root, source_region(root, source, region), source)
        command += ["-c", relative_path(root, source), "-o", output_path(root, source).as_posix()]
        entries.append({"directory": ".", "file": relative_path(root, source),
                        "command": shlex.join(command)})
    output.write_text(json.dumps(entries, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {len(entries)} compile commands to {output}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--region", choices=[*REGION_PATTERNS, "all"], default="all")
    parser.add_argument("--output", type=Path, default=Path("build/vscode/compile_commands.json"))
    parser.add_argument("--root", type=Path, default=Path(__file__).absolute().parents[2])
    args = parser.parse_args()
    return generate(args.root, args.region, args.output)


if __name__ == "__main__":
    raise SystemExit(main())
