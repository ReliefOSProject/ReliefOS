#!/usr/bin/env python3
"""Run the real signed APK updater regressions with host ASan and UBSan.

Build host-runtime-development first, or set RELIEFOS_TEST_DEP_ROOT. Target APK
and BusyBox products are selected by tools/test_system_update.py as usual.
LeakSanitizer cannot inspect /proc inside APK's private chroots; only leak
detection is disabled. Address and undefined-behavior checks remain active.
"""
from pathlib import Path
import os
import re
import shlex
import subprocess
import sys
import tempfile

from runtime_test_dependencies import crypto_flags

ROOT = Path(__file__).resolve().parents[1]


def main():
    temporary = ROOT / "out/tmp"
    temporary.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="system-update-sanitizers-", dir=temporary) as directory:
        binary = Path(directory) / "reliefos-update"
        subprocess.run([
            *shlex.split(os.environ.get("HOSTCC", "cc")), "-std=gnu11", "-O1", "-g",
            "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie",
            *sorted((ROOT / "userland/apps/systemupdate").glob("*.c")),
            ROOT / "tools/tests/system_update_sanitizer_options.c", *crypto_flags(), "-o", binary,
        ], check=True)
        libraries = []
        for line in subprocess.check_output(["ldd", binary], text=True).splitlines():
            match = re.search(r"=>\s+(/\S+)", line) or re.match(r"\s*(/\S+)\s+\(", line)
            if match:
                libraries.append(match.group(1))
        if not libraries:
            raise RuntimeError("host sanitizer binary has no identified runtime libraries")
        environment = {**os.environ, "RELIEFOS_TEST_SYSTEM_UPDATE": str(binary),
                       "RELIEFOS_TEST_SYSTEM_UPDATE_LIBRARIES": ":".join(dict.fromkeys(libraries)),
                       "ASAN_OPTIONS": "detect_leaks=0", "UBSAN_OPTIONS": "halt_on_error=1"}
        return subprocess.run([sys.executable, ROOT / "tools/test_system_update.py", "-v"],
                              env=environment, timeout=300).returncode


if __name__ == "__main__":
    raise SystemExit(main())
