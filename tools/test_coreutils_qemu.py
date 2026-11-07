#!/usr/bin/env python3
"""Check all GNU coreutils and measure validated workloads in a private ReliefOS VM.

The input is the complete, current production VMDK. Only userspace QA fixtures
and an OpenRC service are injected into a private copy; no kernel, runtime library
or coreutils binary is substituted. Timings include process creation, use warm
data and do not imply durable disk throughput.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys
import time

import test_vdso_qemu as disk
from run_gcc_probe_qemu import qmp_quit

ROOT = Path(__file__).resolve().parents[1]
GUEST = "/usr/lib/reliefos/tests/coreutils"
SERVICE = "/etc/init.d/reliefos-coreutils-qa"
LINK = "/etc/runlevels/default/reliefos-coreutils-qa"
LABELS = ("cat", "wc", "sha256sum", "tr", "sort", "cp")


def run(command, **kwargs):
    return subprocess.run([str(item) for item in command], cwd=ROOT, check=True, **kwargs)


def compile_probe(build: Path, output: Path) -> Path:
    # Same pinned musl/mimalloc and target ABI as production userland. This is a
    # standalone acceptance probe, not another production compilation path.
    sysroot = build / "sysroot/musl"
    resource = Path(subprocess.check_output(["clang", "-print-resource-dir"], text=True).strip())
    obj, binary = output / "probe.o", output / "probe"
    run(["clang", "--target=x86_64-linux-musl", "-nostdinc",
         "-isystem", sysroot / "include", "-isystem", resource / "include",
         "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-fno-stack-protector",
         "-mno-avx", "-mno-avx2", "-c", ROOT / "tools/tests/coreutils_probe.c", "-o", obj])
    run(["ld.lld", "-static", "--build-id=none", "-z", "max-page-size=0x1000",
         "--image-base=0x4000000", "-o", binary,
         sysroot / "lib/crt1.o", sysroot / "lib/crti.o", obj,
         sysroot / "lib/mimalloc.o", "--start-group", sysroot / "lib/libc.a",
         "--end-group", sysroot / "lib/crtn.o"])
    return binary


def fixtures(output: Path) -> Path:
    data = output / "data"
    data.mkdir()
    for command, algorithm in (("b2sum", "blake2b"), ("md5sum", "md5"),
                               ("sha1sum", "sha1"), ("sha224sum", "sha224"),
                               ("sha256sum", "sha256"), ("sha384sum", "sha384"),
                               ("sha512sum", "sha512")):
        (data / f"abc.{command}").write_text(hashlib.new(algorithm, b"abc").hexdigest() + "\n")
    checksum = run(["sum"], input=b"abc", capture_output=True).stdout.decode().strip()
    (data / "abc.sum").write_text(checksum + "\n")
    pattern = b"ReliefOS coreutils 0123456789 abcdefghijklmnopqrstuvwxyz\n"
    size = 32 * 1024 * 1024
    bulk = (pattern * (size // len(pattern) + 1))[:size]
    (data / "bulk").write_bytes(bulk)
    (data / "copy-source").write_bytes(bulk[:1024 * 1024])
    (data / "bulk.sha256").write_text(hashlib.sha256(bulk).hexdigest() + "\n")
    (data / "upper.sha256").write_text(hashlib.sha256(bulk.upper()).hexdigest() + "\n")
    (data / "bulk.wc").write_text(f"{bulk.count(bytes([10]))} {len(bulk.split())} {len(bulk)}\n")
    count = 262144
    (data / "numbers").write_text("".join(f"{(i * 65537 + 12345) % count:08d}\n" for i in range(count)))
    sorted_bytes = "".join(f"{i:08d}\n" for i in range(count)).encode()
    (data / "sorted.sha256").write_text(hashlib.sha256(sorted_bytes).hexdigest() + "\n")
    return data


def inject(raw: Path, fs: Path, layout, build: Path, output: Path,
           probe: Path, baseline: Path) -> dict:
    commands, directories = [], set()

    def directory(target):
        if target in directories or disk.debugfs_exists(fs, target):
            return
        directory(str(Path(target).parent))
        commands.append(f"mkdir {target}")
        directories.add(target)

    def put(source, target, mode):
        directory(str(Path(target).parent))
        if disk.debugfs_exists(fs, target):
            raise RuntimeError(f"refuse to replace an existing production file: {target}")
        commands.extend([f'write "{source}" {target}', f"set_inode_field {target} mode {mode}"])

    hashes = {}
    shipped = output / "shipped"
    shipped.mkdir()
    for target, source in (
        ("/usr/bin/coreutils", build / "upstream/coreutils/root/usr/bin/coreutils"),
        ("/usr/libexec/coreutils/libstdbuf.so", build / "upstream/coreutils/root/usr/libexec/coreutils/libstdbuf.so"),
        ("/lib/libc.so", build / "sysroot/musl/lib/libc.so"),
    ):
        copy = shipped / Path(target).name
        run(["debugfs", "-R", f'dump {target} "{copy}"', fs], capture_output=True)
        if disk.file_sha256(copy) != disk.file_sha256(source):
            raise RuntimeError(f"production image differs from the selected build: {target}")
        hashes[target] = disk.file_sha256(copy)
    kernel = shipped / "kernel.sys"
    run(["mcopy", "-i", f'{raw}@@{int(layout["esp_first"]) * 512}', "::/reliefos/kernel.sys", kernel])
    hashes["kernel"] = disk.file_sha256(kernel)
    if hashes["kernel"] != disk.file_sha256(build / "generated/system/kernel.sys"):
        raise RuntimeError("production ESP kernel does not match the selected build")

    put(probe, GUEST + "/probe", "0100755")
    # Consume the kernel-owned clock test instead of duplicating its private
    # vDSO layout here. A trusted high-resolution clock is a benchmark gate.
    vdso_static, _ = disk.compile_probes(output, build / "sysroot/musl")
    put(vdso_static, GUEST + "/vdso-static", "0100755")
    put(baseline, GUEST + "/busybox-benchmark", "0100755")
    put(ROOT / "tools/tests/coreutils_guest.sh", GUEST + "/guest.sh", "0100755")
    put(ROOT / "userland/coreutils/programs.list", GUEST + "/programs.list", "0100644")
    for path in sorted(fixtures(output).iterdir()):
        put(path, GUEST + "/data/" + path.name, "0100644")
    service = output / "service"
    service.write_text(
        '#!/sbin/openrc-run\ndescription="GNU coreutils private acceptance"\n'
        'depend() { need reliefos-runtime; after localmount; before xdm reliefos-session; }\n'
        f'start() {{\n    /bin/bash {GUEST}/guest.sh > /dev/ttyS0 2>&1\n'
        '    status=$?\n    echo "CU_RUNNER_DONE status=$status" > /dev/ttyS0\n'
        '    return 0\n}\n')
    put(service, SERVICE, "0100755")
    directory(str(Path(LINK).parent))
    commands.append(f"symlink {LINK} {SERVICE}")
    command_file = output / "debugfs.commands"
    command_file.write_text("\n".join(commands) + "\n")
    result = run(["debugfs", "-w", "-f", command_file, fs], capture_output=True, text=True)
    (output / "debugfs.log").write_text(result.stdout + result.stderr)
    if re.search(r"File not found|File exists|Could not allocate|No space|write:|mkdir:", result.stdout + result.stderr):
        raise RuntimeError(f"QA injection failed; see {output / 'debugfs.log'}")
    for path in (GUEST + "/probe", GUEST + "/guest.sh", GUEST + "/data/bulk", SERVICE, LINK):
        if not disk.debugfs_exists(fs, path):
            raise RuntimeError(f"injected QA path missing: {path}")
    check = subprocess.run(["e2fsck", "-fn", fs], cwd=ROOT, capture_output=True, text=True)
    (output / "fsck.log").write_text(check.stdout + check.stderr)
    if check.returncode:
        raise RuntimeError(f"private root filesystem check failed: {output / 'fsck.log'}")
    disk.put_root_back(raw, fs, layout)
    return hashes


def boot(command, output, qmp, timeout):
    serial = output / "serial.log"
    with (output / "qemu.log").open("w") as errors:
        process = subprocess.Popen(command, cwd=ROOT, stdout=errors, stderr=errors,
                                   env={**os.environ, "TMPDIR": str(ROOT / "out/tmp")})
        (output / "qemu.pid").write_text(str(process.pid) + "\n")
        deadline, previous, panic_since = time.monotonic() + timeout, "", None
        try:
            while time.monotonic() < deadline:
                text = serial.read_text(errors="replace") if serial.exists() else ""
                markers = [line for line in text.splitlines() if line.startswith("CU_")]
                if markers and markers[-1] != previous:
                    previous = markers[-1]
                    print(previous, flush=True)
                if "KERNEL PANIC" in text and panic_since is None:
                    panic_since = time.monotonic()
                # Let the panic printer finish its register/stack report.
                if "CU_RUNNER_DONE" in text or process.poll() is not None or \
                        (panic_since is not None and time.monotonic() - panic_since >= 5):
                    break
                time.sleep(1)
        finally:
            if process.poll() is None:
                try:
                    qmp_quit(qmp, process)
                except Exception:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)
            if qmp.exists():
                qmp.unlink()
    return serial.read_text(errors="replace") if serial.exists() else ""


def summarize(text: str) -> dict:
    samples, benchmarks = {}, {}
    for match in re.finditer(r"^CU_BENCH (\w+) (\w+) (\d+) (\d+) (\d+) (\d+) (\d+) (\d+)$", text, re.M):
        label, implementation, iteration, size, elapsed, user, system, status = match.groups()
        key = (label, implementation)
        samples.setdefault(key, []).append(dict(iteration=int(iteration), bytes=int(size), elapsed_ns=int(elapsed),
                                               user_us=int(user), system_us=int(system), status=int(status)))
    errors = []
    for label in LABELS:
        benchmarks[label] = {}
        for implementation in ("gnu", "busybox"):
            records = samples.get((label, implementation), [])
            valid = (len(records) == 3 and [sample["iteration"] for sample in records] == [1, 2, 3]
                     and all(sample["status"] == 0 and sample["elapsed_ns"] > 0 for sample in records)
                     and f"CU_BENCH_CHECK {label} {implementation} PASS" in text)
            if not valid:
                errors.append(f"invalid or missing benchmark: {label}/{implementation}")
                continue
            median = statistics.median(sample["elapsed_ns"] for sample in records)
            benchmarks[label][implementation] = {
                "samples": records, "median_ms": median / 1_000_000,
                "median_mib_s": records[0]["bytes"] / (1024 * 1024) / (median / 1_000_000_000),
            }
        if len(benchmarks[label]) == 2:
            benchmarks[label]["gnu_speed_ratio"] = (benchmarks[label]["busybox"]["median_ms"] /
                                                       benchmarks[label]["gnu"]["median_ms"])
    match = re.search(r"^CU_DONE versions=(\d+) functions=(\d+) unsupported=(\d+) failures=(\d+)$", text, re.M)
    counts = dict(zip(("versions", "functions", "unsupported", "failures"), map(int, match.groups()))) if match else {}
    if counts != {"versions": 109, "functions": 109, "unsupported": 2, "failures": 0}:
        errors.append("command acceptance did not pass all 109 entries (including two SELinux diagnostics)")
    if "CU_OWNERSHIP PASS paths=115" not in text or "CU_BUSYBOX_RECOVERY PASS" not in text:
        errors.append("package ownership or explicit BusyBox recovery was not verified")
    if "CU_CLOCK PASS TSC-high-resolution" not in text:
        errors.append("trusted high-resolution benchmark clock was not verified")
    if "CU_RUNNER_DONE status=0" not in text or "KERNEL PANIC" in text or re.search(r"^CU_.* FAIL", text, re.M):
        errors.append("guest failed, panicked or did not finish")
    return {"success": not errors, "counts": counts, "errors": errors, "benchmarks": benchmarks}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="new directory under out/")
    parser.add_argument("--busybox-benchmark", type=Path, required=True, help="optimized BusyBox QA baseline")
    parser.add_argument("--timeout", type=float, default=1800)
    parser.add_argument("--accel", choices=("auto", "kvm", "tcg"), default="auto")
    args = parser.parse_args()
    build, output, baseline = args.build_dir.resolve(), args.output.resolve(), args.busybox_benchmark.resolve()
    image = build / "images/reliefos.vmdk"
    if not output.is_relative_to((ROOT / "out").resolve()) or output.exists():
        parser.error("output must be a new directory under out/")
    if not image.is_file() or not baseline.is_file():
        parser.error("production image and optimized baseline must already be built")
    output.mkdir(parents=True)
    os.environ["TMPDIR"] = str(ROOT / "out/tmp")
    source_hash = disk.file_sha256(image)
    metadata = {"source_image": str(image), "source_image_sha256": source_hash,
                "baseline": str(baseline), "baseline_sha256": disk.file_sha256(baseline),
                "gnu_optimization": "-O2", "busybox_baseline_optimization": "-O2",
                "timing": "CLOCK_MONOTONIC with required TSC-backed vDSO; warmup plus three whole-process samples; no forced persistence"}
    print("Compiling target probe and making private image", flush=True)
    probe = compile_probe(build, output)
    raw, layout = disk.private_raw_image(image, output)
    fs = disk.extract_root(raw, layout, output)
    metadata["shipped_hashes"] = inject(raw, fs, layout, build, output, probe, baseline)
    accelerator = disk.choose_accelerator(args.accel)
    qmp = ROOT.parent / f".reliefos-coreutils-{os.getpid()}.sock"
    # Current ReliefNT deliberately limits trusted TSC timekeeping to one CPU.
    # A PIT-only clock can lose elapsed wall time during long disk commits.
    command = disk.qemu_command(raw, output / "serial.log", qmp, 1, accelerator, None, True)
    metadata.update(accelerator=accelerator, smp=1, memory_mib=4096, qemu_command=command)
    (output / "run.json").write_text(json.dumps(metadata, indent=2) + "\n")
    text = boot(command, output, qmp, args.timeout)
    result = summarize(text)
    result["source_image_unchanged"] = source_hash == disk.file_sha256(image)
    if not result["source_image_unchanged"]:
        result["success"] = False
        result["errors"].append("source production image changed")
    (output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"success": result["success"], "counts": result["counts"], "errors": result["errors"]}, indent=2))
    print(f"Evidence: {output}")
    return 0 if result["success"] else 1


if __name__ == "__main__":
    sys.exit(main())
