#!/usr/bin/env python3
"""Build and run the Linux VT_PROCESS hand-off and EVIOCGRAB contract fixtures.

Both fixtures compile the real kernel sources against a stub harness, so a
failure means the kernel implementation no longer matches the documented Linux
contract rather than that a mock drifted.
"""
from pathlib import Path
import subprocess
import tempfile
import argparse

ROOT = Path(__file__).resolve().parents[1]

PTY_INCLUDES = [
    "-Ikernel/reliefnt/include", "-Iinclude",
    "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/kernel/reliefnt/include",
]

FIXTURES = [
    # (source, extra flags, description)
    ("tools/tests/xorg_vt_handoff_test.c", [],
     "xorg-vt-handoff"),
    ("tools/tests/evdev_grab_test.c", [],
     "evdev-grab"),
]


CASES = {
    "xorg-vt-handoff": ["controller", "reldisp", "occupancy", "bitmap", "death", "detach",
                         "nonleader-death", "keyboard", "raw-modifier", "graphics-keyboard",
                         "modifier-transition", "auto-graphics", "raw-termios", "missing-controller"],
    "evdev-grab": ["scalar", "repeat", "release", "console", "publication", "ofd", "ioctl-query", "vt-bind"],
}


def run(source: str, extra: list[str], name: str, selected: str | None) -> bool:
    with tempfile.TemporaryDirectory(prefix=f"reliefos-{name}-") as directory:
        binary = Path(directory) / name
        subprocess.run(
            ["clang", "-std=c11", "-g", "-O1",
             "-ffunction-sections", "-fdata-sections",
             "-fsanitize=address,undefined",
             "-fno-sanitize-recover=all",
             *PTY_INCLUDES, *extra,
             "-Wl,--gc-sections", source, "-o", str(binary)],
            cwd=ROOT, check=True)
        passed = True
        for case in ([selected] if selected else [*CASES[name], "full"]):
            if case not in CASES[name] and case != "full":
                continue
            result = subprocess.run([str(binary), *([] if case == "full" else [case])],
                                    cwd=ROOT, timeout=30, capture_output=True, text=True)
            print(result.stdout, end="")
            if result.returncode:
                passed = False
                print(f"FAIL {name}/{case} exit={result.returncode}\n{result.stderr}")
        return passed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--case")
    args = parser.parse_args()
    passed = True
    for source, extra, name in FIXTURES:
        print(f"== {name} ==")
        passed = run(source, extra, name, args.case) and passed
    print("xorg-vt-handoff: " + ("PASS" if passed else "FAIL"))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
