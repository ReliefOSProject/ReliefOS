#!/usr/bin/env python3
"""Benchmark demand-zero faults and check parallel processes/CLONE_VM in ReliefOS."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

import test_apk_qemu as runner
from make_ext2_root import write_ext2_root
from make_live_root import make_live_tree

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=ROOT / "build/execution-lock-qemu")
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--scheduler", action="store_true", help="run CPU fairness/affinity probe")
    args = parser.parse_args()
    work = args.out.resolve()
    work.mkdir(parents=True, exist_ok=True)
    executable = work / "probe"
    subprocess.run([str(ROOT / "build/musl/sdk/bin/reliefos-musl-cc"), "-static", "-O2",
                    "-pthread", str(ROOT / "kernel/reliefnt/tools/tests" / (
                        "eevdf_runtime_probe.c" if args.scheduler else "execution_lock_runtime_probe.c")),
                    "-o", str(executable)], check=True)
    if not args.scheduler:
        subprocess.run([str(executable)], check=True, timeout=60)
    with tempfile.TemporaryDirectory(prefix="stage-", dir=work) as tmp:
        stage = Path(tmp) / "root"
        make_live_tree(ROOT / "build/apk/root", stage)
        tests = stage / "usr/lib/reliefos/tests"
        tests.mkdir(parents=True, exist_ok=True)
        shutil.copy2(executable, tests / "linux-inventory.elf")
        write_ext2_root(stage, work / "root.ext2", minimum_mib=512)
    iso = runner.iso_tools
    iso.GRUB_TEMPLATE = iso.GRUB_TEMPLATE.replace(
        "autospawn=ioctlcloexec autospawn=python315", "autospawn=inventory").replace(
        "syscall-trace=/opt/python/", "").replace("set timeout=5", "set timeout=0")
    iso.build_iso(work / "root.ext2", work / "reliefos-apk.iso", work / "grub.cfg", work)
    runner.WORK = work
    runner.guest(args.timeout)
    serial = (work / "guest-serial.log").read_text(errors="replace")
    if args.scheduler:
        print("\n".join(line for line in serial.splitlines() if "[eevdf-probe]" in line))
        assert len(re.findall(r"\[eevdf-probe\] nice=", serial)) == 2
        return
    measurements = [dict(processes=int(p), pages=int(n), seconds=float(s))
                    for p, n, s in re.findall(
                        r"\[execution-bench\] processes=(\d+) pages=(\d+) seconds=([\d.]+)", serial)]
    assert len(measurements) == 2, serial[-4000:]
    result = {"kernel_sha256": hashlib.sha256((work / "iso/leonos/kernel.sys").read_bytes()).hexdigest(),
              "platform": "QEMU/KVM, 2 vCPUs, 4096 MiB", "measurements": measurements}
    (work / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
