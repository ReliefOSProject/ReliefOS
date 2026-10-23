#!/usr/bin/env python3
"""Compare pinned musl secure-exec against Linux 6.12 and a private ReliefOS disk."""
import argparse
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists() or not output.is_relative_to(ROOT / "build"):
        parser.error("--output must be a new directory under build/")
    output.mkdir(parents=True)
    compiler = json.loads((ROOT / "build/auth-upstream/linux-pam-build.json").read_text())["compiler"]
    module = ROOT / "tools/tests/secure_loader_module.c"
    files = []
    for name, definition, destination in (
            ("trusted", "DEPENDENCY_VALUE=1", "/lib/libreliefos-security-probe.so"),
            ("hostile", "DEPENDENCY_VALUE=2", "/tmp/libreliefos-security-probe.so"),
            ("preload", "HOSTILE_PRELOAD", "/tmp/hostile-preload.so")):
        path = output / name
        path.mkdir()
        binary = path / Path(destination).name
        subprocess.run([*compiler, "-shared", "-fPIC", "-O2", "-D" + definition,
                        str(module), "-o", str(binary)], check=True)
        files += ["--file", str(binary), destination]
    binary = output / "secure-exec-probe.elf"
    subprocess.run([*compiler, "-O2", "-DSECURE_LOADER_TEST",
                    str(ROOT / "kernel/reliefnt/tools/tests/setid_runtime_probe.c"),
                    "-L" + str(output / "trusted"), "-lreliefos-security-probe", "-o", str(binary)], check=True)
    subprocess.run(["python3", "tools/test_linux_reference.py", "--probe-binary", str(binary),
                    "--case", "secure_loader", "--log", str(output / "linux-6.12.log"),
                    "--file", str(ROOT / "build/musl/sysroot/lib/libc.so"), "/lib/ld-musl-x86_64.so.1",
                    *files], cwd=ROOT, check=True)
    subprocess.run(["python3", "tools/test_auth_guest.py", "--output", str(output / "guest"),
                    "--smp", "2", "--probe", str(binary), "--marker", "setid", *files], cwd=ROOT, check=True)


if __name__ == "__main__":
    main()
