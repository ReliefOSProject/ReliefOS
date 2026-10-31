#!/usr/bin/env python3
"""Prepare and measure a private ReliefOS SMP workload image."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_gcc_probe_qemu import qmp_quit
import test_vdso_qemu as disk

ROOT = Path(__file__).resolve().parents[1]
NATIVE_SOURCE = ROOT / "kernel/reliefnt/tools/tests/smp_latency_probe.c"
X11_SOURCE = ROOT / "tools/tests/smp_x11_latency_probe.c"
GUEST_SOURCE = ROOT / "tools/tests/smp_latency_guest.sh"
GUEST_DIR = "/usr/lib/reliefos/tests/smp-latency"
SERVICE = "/etc/init.d/reliefos-smp-latency"
SERVICE_LINK = "/etc/runlevels/default/reliefos-smp-latency"


def stop_owned(process, socket_path: Path) -> None:
    if process.poll() is None:
        try:
            qmp_quit(socket_path, process)
        except Exception:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
    else:
        process.wait(timeout=5)

CASES = ("idle", "compile", "ffmpeg", "native")
VERSION_TOOLS = ("gcc", "make", "as", "ld", "ffmpeg", "ffprobe")
SCOPES = {"generic", "syscall_io", "syscall_process", "syscall_memory",
          "syscall_other", "fault", "sched", "resumed"}
COUNTERS = ("acquisitions", "contentions", "wait_total", "wait_max", "hold_total",
            "hold_max", "irq_total", "irq_max", "clock_errors")
HISTS = ("wait_hist", "hold_hist", "irq_hist")
HEADER = re.compile(r"reliefnt_lock_stats 1 unit=tsc_cycles enabled=0 epoch=(\d+) "
                    r"histogram=log2_cycles irq=gate_masked_window")
FATAL = re.compile(r"(?i)kernel panic|\[panic\]|bugcheck")
NATIVE_LEVEL = re.compile(r"^\[ *[0-9]+\.[0-9]{6}\] "
                          r"(?:\x1b\[38;2;[0-9]{1,3};[0-9]{1,3};[0-9]{1,3}m)?\[")
# Native console prefixes from drivers/console/log.c; errors are never elided.
KERNEL_CONSOLE = re.compile(
    r"\[ *[0-9]+\.[0-9]{6}\] "
    r"(?:\x1b\[38;2;[0-9]{1,3};[0-9]{1,3};[0-9]{1,3}m\[(?:DEBUG|INFO)\] \x1b\[0m|\[(?:DEBUG|INFO)\] )"
    r"\[reliefnt\] [^\x00-\x1f\x7f]*"
)


def generate_fixture(directory: Path) -> list[Path]:
    """Generate a fixed 32-TU, bounded compile workload with a fixed oracle."""
    directory.mkdir(parents=True, exist_ok=True)
    generated = []
    for unit in range(32):
        path = directory / f"unit{unit:02d}.c"
        lines = ["#include <stdint.h>", f"uint64_t unit{unit:02d}(uint64_t x) {{"]
        for step in range(512):
            constant = (unit + 1) * 0x9e3779b1 + step * 0x85ebca6b
            lines.append(f"x ^= x << 13; x ^= x >> 7; x ^= x << 17; x += UINT64_C({constant});")
        path.write_text("\n".join(lines + ["return x;", "}", ""]))
        generated.append(path)
    main = directory / "main.c"
    declarations = "\n".join(f"uint64_t unit{n:02d}(uint64_t);" for n in range(32))
    calls = "\n".join(f"    result ^= unit{n:02d}(seed ^ {n});" for n in range(32))
    main.write_text("#include <inttypes.h>\n#include <stdint.h>\n#include <stdio.h>\n" + declarations +
                    "\nint main(void) {\n    volatile uint64_t seed = UINT64_C(0x123456789abcdef0);\n"
                    "    uint64_t result = 0;\n" + calls +
                    "\n    printf(\"%016\" PRIx64 \"\\n\", result);\n    return 0;\n}\n")
    makefile = directory / "Makefile"
    objects = " ".join(f"unit{n:02d}.o" for n in range(32))
    makefile.write_text(f"CC = gcc\nCFLAGS = -std=c11 -O2 -Wall -Wextra -Werror\n"
                        f"OBJECTS = main.o {objects}\n"
                        "all: fixture\nfixture: $(OBJECTS)\n\t$(CC) $(CFLAGS) -o $@ $(OBJECTS)\n"
                        "clean:\n\trm -f $(OBJECTS) fixture\n.PHONY: all clean\n")
    return generated + [main, makefile]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def verify_source_hashes(frozen: dict[str, str]) -> None:
    for name, digest in frozen.items():
        if sha256(Path(name)) != digest:
            raise ValueError(f"input changed during preparation: {name}")


def checked_member(base: Path, name: str) -> Path:
    member = Path(name)
    if member.is_absolute() or not member.parts or any(part in (".", "..") for part in member.parts):
        raise ValueError("unsafe cohort member path")
    path = base / member
    if path.is_symlink() or not path.is_file() or not path.resolve().is_relative_to(base.resolve()):
        raise ValueError(f"missing or unsafe cohort member: {name}")
    return path


def sums(path: Path) -> dict[str, str]:
    result = {}
    for line in path.read_text().splitlines():
        match = re.fullmatch(r"([a-f0-9]{64})  ([^\n]+)", line)
        if not match or match[2] in result:
            raise ValueError(f"invalid checksum list: {path}")
        result[match[2]] = match[1]
    return result


def pin_member(path: Path) -> dict:
    before = path.lstat()
    if not stat.S_ISREG(before.st_mode):
        raise ValueError(f"cohort member is not a regular file: {path}")
    identity = (before.st_dev, before.st_ino, before.st_mode, before.st_size,
                before.st_mtime_ns, before.st_ctime_ns)
    digest = sha256(path)
    after = path.lstat()
    if identity != (after.st_dev, after.st_ino, after.st_mode, after.st_size,
                    after.st_mtime_ns, after.st_ctime_ns):
        raise ValueError(f"cohort member changed during hashing: {path}")
    return {"identity": identity, "sha256": digest}


def verify_cohort_pins(cohort: dict) -> None:
    for name, pinned in cohort["pins"].items():
        try:
            if pin_member(Path(name)) != pinned:
                raise ValueError(f"cohort member changed during preparation: {name}")
        except OSError as error:
            raise ValueError(f"cohort member unavailable: {name}") from error


def environment_values(env: bytes) -> dict[str, str]:
    # Use the same environment-block parser as the boot tooling, over the exact
    # bytes that will be interpreted below; timeout bounds the host subprocess.
    try:
        with tempfile.TemporaryDirectory(prefix="smp-grub-env-") as directory:
            path = Path(directory) / "boot.env"
            path.write_bytes(env)
            result = subprocess.run(["grub-editenv", str(path), "list"], stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, timeout=5)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise ValueError(f"cannot validate GRUB environment: {error}") from error
    if result.returncode:
        raise ValueError("invalid GRUB environment block")
    values = {}
    for line in result.stdout.decode("ascii").splitlines():
        if line and not line.startswith("#") and "=" in line:
            key, value = line.split("=", 1)
            if key in values:
                raise ValueError("duplicate boot environment key")
            values[key] = value
    return values


def validate_cohort(root: Path, esp: Path, selected_kernel: Path) -> dict:
    try:
        manifest_path = checked_member(root, "kernel-payload.json")
        manifest_bytes = manifest_path.read_bytes()
        manifest = json.loads(manifest_bytes)
        release, abi = manifest["release"], manifest["module_abi"]
        if manifest["schema"] != 2 or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+-]{11,30}", release) or \
                not re.fullmatch(r"[a-f0-9]{64}", abi) or release[-12:] != abi[:12]:
            raise ValueError("invalid cohort identity")
        prefix = f"usr/lib/reliefos/kernel/{release}"
        moddir = f"lib/modules/{release}"
        if (manifest["kernel"], manifest["initramfs"], manifest["module_directory"]) != \
                (f"{prefix}/kernel.sys", f"{prefix}/initramfs.cpio", moddir):
            raise ValueError("inconsistent cohort paths")
        members = manifest["files"]
        essential = {manifest["kernel"], manifest["initramfs"], f"{prefix}/boot.env",
                     f"{prefix}/SHA256SUMS", f"{moddir}/module.abi", f"{moddir}/modules.dep",
                     "usr/lib/reliefos/kernel/current-release"}
        if not isinstance(members, dict) or not essential <= members.keys():
            raise ValueError("incomplete root payload")
        isha = members[manifest["initramfs"]]["sha256"]
        if not re.fullmatch(r"[a-f0-9]{64}", isha):
            raise ValueError("invalid initramfs digest")
        version = f"reliefos/versions/{release}/{isha}"
        pin_esp = [f"{version}/{name}" for name in ("kernel.sys", "initramfs.cpio", "boot.env", "boot.json", "SHA256SUMS")]
        pin_esp += [f"reliefos/state/kernel-{slot}.{suffix}" for slot in ("A", "B") for suffix in ("env", "sha256")]
        pin_esp += [f"{brand}/{name}" for brand in ("reliefos", "leonos") for name in ("kernel.sys", "initramfs.cpio")]
        paths = [checked_member(root, name) for name in members]
        paths += [checked_member(root, "kernel-payload.json"), checked_member(root, f"{prefix}/bundle.json")]
        paths += [checked_member(esp, name) for name in pin_esp] + [selected_kernel]
        pins = {str(path.absolute()): pin_member(path) for path in paths}
        if pins[str(manifest_path.absolute())]["sha256"] != hashlib.sha256(manifest_bytes).hexdigest():
            raise ValueError("manifest changed while reading")
        for tree in (prefix, moddir):
            actual = {path.relative_to(root).as_posix() for path in (root / tree).rglob("*")
                      if path.is_file() or path.is_symlink()}
            listed = {name for name in members if name.startswith(tree + "/")}
            if tree == prefix: listed.add(f"{prefix}/bundle.json")
            if actual != listed:
                raise ValueError(f"unlisted or missing cohort files in {tree}")
        if checked_member(root, f"{prefix}/bundle.json").read_bytes() != checked_member(root, "kernel-payload.json").read_bytes():
            raise ValueError("root bundle manifest mismatch")
        for name, record in members.items():
            if not (name.startswith(prefix + "/") or name.startswith(moddir + "/") or
                    name == "usr/lib/reliefos/kernel/current-release"):
                raise ValueError("root payload member outside cohort")
            path = checked_member(root, name)
            if path.stat().st_size != record["size"] or sha256(path) != record["sha256"]:
                raise ValueError(f"root payload mismatch: {name}")
        if checked_member(root, f"{moddir}/module.abi").read_text().strip() != abi or \
                checked_member(root, "usr/lib/reliefos/kernel/current-release").read_text().strip() != release:
            raise ValueError("root module or release identity mismatch")
        if sha256(selected_kernel) != members[manifest["kernel"]]["sha256"]:
            raise ValueError("selected kernel does not match payload")
        listed = sums(checked_member(root, f"{prefix}/SHA256SUMS"))
        expected = {name: record["sha256"] for name, record in members.items()
                    if name not in {f"{prefix}/SHA256SUMS", "usr/lib/reliefos/kernel/current-release"}}
        if listed != expected:
            raise ValueError("root checksum manifest mismatch")
        ksha = members[manifest["kernel"]]["sha256"]
        isha = members[manifest["initramfs"]]["sha256"]
        version = f"reliefos/versions/{release}/{isha}"
        boot = json.loads(checked_member(esp, f"{version}/boot.json").read_text())
        if any(boot.get(key) != value for key, value in {"schema": 1, "release": release,
                "module_abi": abi, "kernel_sha256": ksha, "initramfs_sha256": isha}.items()):
            raise ValueError("ESP boot identity mismatch")
        env = checked_member(root, f"{prefix}/boot.env").read_bytes()
        values = environment_values(env)
        boot_identity = {"reliefos_release": release, "reliefos_module_abi": abi,
                         "reliefos_kernel_sha256": ksha, "reliefos_initramfs_sha256": isha}
        for key, value in {**boot_identity, "reliefos_generation": "1"}.items():
            if values.get(key) != value:
                raise ValueError("boot environment identity mismatch")
        env_sha = hashlib.sha256(env).hexdigest()
        version_sums = sums(checked_member(esp, f"{version}/SHA256SUMS"))
        if version_sums != {"kernel.sys": ksha, "initramfs.cpio": isha, "boot.env": env_sha}:
            raise ValueError("ESP checksum manifest mismatch")
        esp_files = [f"{version}/{name}" for name in ("kernel.sys", "initramfs.cpio", "boot.env", "boot.json", "SHA256SUMS")]
        for name, digest in version_sums.items():
            if sha256(checked_member(esp, f"{version}/{name}")) != digest:
                raise ValueError("ESP version payload mismatch")
        for slot in ("A", "B"):
            name = f"kernel-{slot}.env"
            env_path = checked_member(esp, f"reliefos/state/{name}")
            sum_path = checked_member(esp, f"reliefos/state/kernel-{slot}.sha256")
            slot_env = env_path.read_bytes()
            slot_values = environment_values(slot_env)
            generation = slot_values.get("reliefos_generation", "")
            if (not re.fullmatch(r"[1-9][0-9]{0,9}", generation) or int(generation) > 2147483647 or
                    any(slot_values.get(key) != value for key, value in boot_identity.items()) or
                    sums(sum_path) != {name: hashlib.sha256(slot_env).hexdigest()}):
                raise ValueError("ESP slot mismatch")
            esp_files += [f"reliefos/state/{name}", f"reliefos/state/kernel-{slot}.sha256"]
        for brand in ("reliefos", "leonos"):
            for name, digest in (("kernel.sys", ksha), ("initramfs.cpio", isha)):
                member = f"{brand}/{name}"
                if sha256(checked_member(esp, member)) != digest:
                    raise ValueError("ESP alias mismatch")
                esp_files.append(member)
        result = {"release": release, "module_abi": abi, "kernel_sha256": ksha,
                  "initramfs_sha256": isha, "manifest": manifest, "esp_files": esp_files, "pins": pins}
        verify_cohort_pins(result)
        return result
    except (OSError, KeyError, TypeError, json.JSONDecodeError, UnicodeError) as error:
        raise ValueError(f"invalid cohort: {error}") from error


def parse_stats(lines: list[str], required: bool) -> dict:
    if not lines or not (header := HEADER.fullmatch(lines[0])):
        raise ValueError("missing or invalid disabled profiler header")
    rows = []
    seen = set()
    for line in lines[1:]:
        fields = line.split(" ")
        parsed = {}
        for field in fields:
            if field.count("=") != 1:
                raise ValueError("invalid profiler field")
            key, value = field.split("=", 1)
            if key in parsed:
                raise ValueError("duplicate profiler field")
            parsed[key] = value
        if set(parsed) != {"cpu", "scope", *COUNTERS, *HISTS}:
            raise ValueError("invalid profiler columns")
        if not parsed["cpu"].isdecimal() or parsed["scope"] not in SCOPES:
            raise ValueError("invalid profiler CPU or scope")
        key = (int(parsed["cpu"]), parsed["scope"])
        if key in seen:
            raise ValueError("duplicate profiler CPU/scope")
        seen.add(key)
        for field in COUNTERS:
            if not parsed[field].isdecimal():
                raise ValueError("invalid profiler counter")
            parsed[field] = int(parsed[field])
            if parsed[field] > (1 << 64) - 1:
                raise ValueError("profiler counter exceeds uint64")
        if parsed["clock_errors"]:
            raise ValueError("profiler clock error; timing is invalid")
        for field in HISTS:
            bins = parsed[field].split(",")
            if len(bins) != 32 or any(not item.isdecimal() for item in bins):
                raise ValueError("invalid profiler histogram")
            parsed[field] = [int(item) for item in bins]
            if any(value > (1 << 64) - 1 for value in parsed[field]):
                raise ValueError("profiler histogram exceeds uint64")
        maximum = (1 << 64) - 1
        acquisitions = parsed["acquisitions"]
        if parsed["contentions"] > acquisitions:
            raise ValueError("contentions exceed acquisitions")
        for kind in ("wait", "hold", "irq"):
            count = min(sum(parsed[kind + "_hist"]), maximum)
            if parsed[kind + "_total"] < parsed[kind + "_max"]:
                raise ValueError("profiler maximum exceeds total")
            if (kind == "wait" and count != acquisitions) or (kind != "wait" and count > acquisitions):
                raise ValueError("profiler sample count inconsistent with acquisitions")
            if not count and (parsed[kind + "_total"] or parsed[kind + "_max"]):
                raise ValueError("profiler timing has no sample")
        parsed["cpu"] = key[0]
        rows.append(parsed)
    if required and (not rows or not any(row["acquisitions"] for row in rows)):
        raise ValueError("profiling case has no sampled acquisitions")
    return {"epoch": int(header[1]), "unit": "tsc_cycles", "rows": rows}


def parse_x11(lines: list[str]) -> dict:
    if not lines:
        raise ValueError("missing GUI output")
    summary = re.fullmatch(r"X11 count=(\d+) p50_ns=(\d+) p95_ns=(\d+) p99_ns=(\d+) max_ns=(\d+) wm=1 pixels=1", lines[0])
    if not summary:
        raise ValueError("invalid GUI summary")
    count, p50, p95, p99, maximum = map(int, summary.groups())
    samples = []
    for line in lines[1:]:
        match = re.fullmatch(r"X11_SAMPLE ns=(\d+)", line)
        if not match:
            raise ValueError("invalid GUI sample")
        samples.append(int(match[1]))
    if count != len(samples) or not count or count > 30000:
        raise ValueError("GUI sample count mismatch")
    ordered = sorted(samples)
    rank = lambda p: ordered[(count * p + 99) // 100 - 1]
    if (p50, p95, p99, maximum) != (rank(50), rank(95), rank(99), ordered[-1]):
        raise ValueError("GUI sample quantiles mismatch")
    return {"count": count, "samples_ns": samples, "p50_ns": p50,
            "p95_ns": p95, "p99_ns": p99, "max_ns": maximum}


def parse_native(lines: list[str]) -> list[dict]:
    if not lines:
        raise ValueError("missing native pattern")
    pattern = re.fullmatch(r"NATIVE_PATTERN page0=(\d+) page1=(\d+) page256=(\d+)", lines[0])
    if not pattern or len(set(pattern.groups())) != 3:
        raise ValueError("native file pages are not distinct")
    rows = []
    for line in lines[1:]:
        match = re.fullmatch(r"NATIVE kind=(demand|cow|shared|filemap|syscall) count=(\d+) "
                             r"p50_ns=(\d+) p95_ns=(\d+) p99_ns=(\d+) max_ns=(\d+)", line)
        if not match:
            raise ValueError("invalid native probe output")
        kind, *values = match.groups()
        count, p50, p95, p99, maximum = map(int, values)
        if count != 2048 or not p50 <= p95 <= p99 <= maximum:
            raise ValueError("invalid native probe count or quantiles")
        rows.append({"kind": kind, "count": count, "p50_ns": p50, "p95_ns": p95,
                     "p99_ns": p99, "max_ns": maximum})
    if len(rows) != 5 or {row["kind"] for row in rows} != {"demand", "cow", "shared", "filemap", "syscall"}:
        raise ValueError("native workload incomplete")
    return rows


def parse_serial(serial: str, nonce: str, release: str, modes: list[str],
                 repeat: int, min_gui_samples: int = 10) -> dict:
    if not re.fullmatch(r"[a-zA-Z0-9]+", nonce) or not modes or repeat < 1:
        raise ValueError("invalid expected run identity")
    if len(set(modes)) != len(modes) or set(modes) - {"on", "off", "absent"}:
        raise ValueError("invalid profiler expectation")
    if FATAL.search(serial):
        raise ValueError("guest fatal marker")
    expected = {(mode, case, rep) for mode in modes for case in CASES
                for rep in range(1, repeat + 1)}
    rows, snapshots, gui_data, native_data, absent = {}, {}, {}, {}, set()
    versions = {}
    active_version = None
    started = done = False
    active_key = None
    active_kind = None
    active_lines = []
    for raw in serial.splitlines(keepends=True):
        complete = raw.endswith("\n")
        line = raw.rstrip("\r\n")
        if prefix := NATIVE_LEVEL.match(line):
            severity = line[prefix.end():]
            levels = ("DEBUG", "INFO", "WARN", "ERROR", "FATAL")
            if (not severity or any(level.startswith(severity) for level in levels)) or \
                    (not complete and any(severity.startswith(level + "]") for level in levels)):
                raise ValueError("truncated native severity line")
            if severity.startswith("FATAL]") or (started and not done and severity.startswith("ERROR]")):
                raise ValueError("guest native severity failure")
        if not complete and (line.startswith("SMP_") or (line and "SMP_".startswith(line))):
            raise ValueError("truncated marker")
        if (active_key is not None or active_version is not None) and complete and \
                "SMP_" not in line and KERNEL_CONSOLE.fullmatch(line):
            continue
        if active_version is not None and not line.startswith("SMP_VERSION_"):
            raise ValueError("interrupted version output")
        if active_key is not None and not line.startswith(f"SMP_{active_kind}_END:"):
            if line.startswith("SMP_"):
                raise ValueError("nested or malformed telemetry")
            if not complete:
                raise ValueError("truncated telemetry")
            active_lines.append(line)
            continue
        if not line.startswith("SMP_"):
            continue
        if line == f"SMP_START:{nonce}:{release}" and not started and not done:
            started = True
            continue
        if line == f"SMP_DONE:{nonce}:failures=0" and started and not done:
            done = True
            continue
        if not started or done:
            raise ValueError("marker outside run")
        if line.startswith("SMP_VERSION_"):
            match = re.fullmatch(r"SMP_VERSION_LINE:([^:]+):([^:]+):(\d+):(.*)", line)
            if match:
                marker_nonce, name, number, content = match.groups()
                if marker_nonce != nonce or name not in VERSION_TOOLS or rows or \
                        (active_version is not None and active_version != name) or \
                        (name in versions and versions[name]["status"] is not None):
                    raise ValueError("unexpected version output")
                if name not in versions:
                    versions[name] = {"lines": [], "status": None}
                if int(number) != len(versions[name]["lines"]) + 1:
                    raise ValueError("missing or duplicate version line")
                versions[name]["lines"].append(content)
                active_version = name
                continue
            match = re.fullmatch(r"SMP_VERSION_END:([^:]+):([^:]+):(\d+):(\d+)", line)
            if not match:
                raise ValueError("invalid version marker")
            marker_nonce, name, status, count = match.groups()
            if marker_nonce != nonce or name != active_version or name not in versions or \
                    int(status) != 0 or int(count) != len(versions[name]["lines"]) or not int(count):
                raise ValueError("failed or incomplete version output")
            versions[name]["status"] = int(status)
            active_version = None
            continue
        match = re.fullmatch(r"SMP_(STATS_BEGIN|STATS_END|X11_BEGIN|X11_END|NATIVE_BEGIN|NATIVE_END|ABSENT):([^:]+):"
                             r"(on|off|absent):([a-z]+):(\d+)", line)
        if match:
            kind, marker_nonce, mode, case, number = match.groups()
            key = (mode, case, int(number))
            if marker_nonce != nonce or key not in expected:
                raise ValueError("unexpected telemetry identity")
            if kind == "STATS_BEGIN" and active_key is None and key not in snapshots and mode != "absent":
                active_key, active_kind, active_lines = key, "STATS", []
            elif kind == "X11_BEGIN" and active_key is None and key not in gui_data:
                active_key, active_kind, active_lines = key, "X11", []
            elif kind == "NATIVE_BEGIN" and active_key is None and key not in native_data and case == "native":
                active_key, active_kind, active_lines = key, "NATIVE", []
            elif kind == f"{active_kind}_END" and key == active_key:
                if active_kind == "STATS":
                    snapshots[key] = parse_stats(active_lines, required=mode == "on")
                    if mode == "off" and snapshots[key]["rows"]:
                        raise ValueError("sampling-off snapshot unexpectedly contains acquisitions")
                elif active_kind == "X11": gui_data[key] = parse_x11(active_lines)
                elif active_kind == "NATIVE": native_data[key] = parse_native(active_lines)
                active_key, active_kind = None, None
            elif kind == "ABSENT" and mode == "absent" and key not in absent:
                absent.add(key)
            else:
                raise ValueError("duplicate or invalid telemetry marker")
            continue
        match = re.fullmatch(r"SMP_ROW:([^:]+):(on|off|absent):([a-z]+):(\d+):"
                             r"elapsed_ns=(\d+):work=(\d+):check=(\d+):gui=(\d+):gui_samples=(\d+)", line)
        if not match:
            raise ValueError("invalid run marker")
        marker_nonce, mode, case, number, elapsed, work, check, gui, samples = match.groups()
        key = (mode, case, int(number))
        elapsed, work, check, gui, samples = map(int, (elapsed, work, check, gui, samples))
        if marker_nonce != nonce or key not in expected or key in rows:
            raise ValueError("unexpected or duplicate result row")
        if not elapsed or work or check or gui or samples < min_gui_samples:
            raise ValueError("workload, correctness or GUI check failed")
        rows[key] = {"mode": mode, "case": case, "repeat": key[2],
                     "elapsed_ns": elapsed, "work": work, "check": check,
                     "gui_status": gui, "gui_samples": samples}
    if not started or not done or active_key is not None or active_version is not None or set(rows) != expected:
        raise ValueError("incomplete run")
    if set(versions) != set(VERSION_TOOLS) or any(version["status"] != 0 for version in versions.values()):
        raise ValueError("missing or failed guest tool versions")
    if set(snapshots) != {key for key in expected if key[0] != "absent"} or \
            absent != {key for key in expected if key[0] == "absent"}:
        raise ValueError("missing profiler evidence")
    if set(gui_data) != expected or set(native_data) != {key for key in expected if key[1] == "native"}:
        raise ValueError("missing GUI or native evidence")
    if any(rows[key]["gui_samples"] != gui_data[key]["count"] for key in expected):
        raise ValueError("GUI row count differs from distribution")
    return {"release": release, "nonce": nonce, "latency_unit": "nanoseconds", "versions": versions,
            "rows": [dict(rows[key], telemetry=snapshots.get(key), gui=gui_data[key],
                          native=native_data.get(key)) for key in sorted(rows)]}


def run(command, **kwargs):
    env = {**os.environ, "TMPDIR": str(ROOT / "out/tmp")}
    return subprocess.run([str(part) for part in command], cwd=ROOT, env=env, check=True, **kwargs)


def compile_probes(out: Path, sysroot: Path, sdk: Path, x11: Path, log) -> tuple[Path, Path]:
    compiler = sdk / "bin/reliefos-musl-cc"
    for member in ("lib/libc.a", "include/stdio.h", "include/pthread.h"):
        if sha256(sdk / member) != sha256(sysroot / member):
            raise ValueError(f"SDK and selected sysroot differ: {member}")
    native, gui = out / "smp-native", out / "smp-x11"
    run([compiler, "-static", "-pthread", "-O2", "-Wall", "-Wextra", "-Werror",
         NATIVE_SOURCE, "-o", native], stdout=log, stderr=log)
    run([compiler, "-O2", "-Wall", "-Wextra", "-Werror", "-isystem", x11 / "usr/include",
         X11_SOURCE, "-L" + str(x11 / "usr/lib"), "-lX11", "-o", gui], stdout=log, stderr=log)
    native_headers = subprocess.check_output(["readelf", "-l", str(native)], text=True)
    gui_headers = subprocess.check_output(["readelf", "-l", str(gui)], text=True)
    if "INTERP" in native_headers or "/lib/ld-musl-x86_64.so.1" not in gui_headers:
        raise ValueError("probe linked against an unexpected runtime")
    return native, gui


def quote_debugfs(path: Path) -> str:
    value = str(path)
    if any(c in value for c in ('"', "\\", "\n", "\r", "\x00")):
        raise ValueError("unsupported character in debugfs source path")
    return '"' + value + '"'


def check_esp(raw: Path, first: int, last: int, output: Path, log=None) -> None:
    if first < 0 or last < first:
        raise ValueError("invalid ESP extent")
    remaining = (last - first + 1) * 512
    with raw.open("rb") as image, output.open("wb") as esp:
        image.seek(first * 512)
        while remaining:
            block = image.read(min(remaining, 8 * 1024 * 1024))
            if not block: raise ValueError("truncated ESP extent")
            esp.write(block); remaining -= len(block)
    with output.open("rb") as esp:
        esp.seek(510)
        if esp.read(2) != b"\x55\xaa":
            raise ValueError("invalid FAT boot sector signature")
    result = subprocess.run(["fsck.fat", "-n", str(output)], stdout=log or subprocess.PIPE,
                            stderr=log or subprocess.STDOUT, text=True)
    if result.returncode:
        raise ValueError(f"ESP filesystem check failed: {result.returncode}")


def validate_image_metadata(info: dict, image: Path) -> str:
    if info.get("backing-filename") or info.get("full-backing-filename"):
        raise ValueError("backing images cannot be represented by a single source hash")
    kind = info.get("format")
    if kind == "raw": return kind
    if kind != "vmdk": raise ValueError("source image must be raw or VMDK")
    extents = info.get("format-specific", {}).get("data", {}).get("extents", [])
    if not extents or any(Path(extent.get("filename", "")).resolve() != image.resolve() for extent in extents):
        raise ValueError("VMDK external extents cannot be represented by a single source hash")
    return kind


def validate_partitions(raw: Path, partitions: list[dict]) -> list[dict]:
    size = raw.stat().st_size
    if not size or size % 512:
        raise ValueError("invalid disk size")
    extents = []
    for part in partitions:
        first, last = part["first"], part["last"]
        if type(first) is not int or type(last) is not int or first < 2 or last < first or last >= size // 512:
            raise ValueError("invalid GPT partition extent")
        extents.append((first, last))
    extents.sort()
    if any(previous[1] >= following[0] for previous, following in zip(extents, extents[1:])):
        raise ValueError("overlapping GPT partitions")
    for name in ("RELIEFOS_ESP", "RELIEFOS_ROOT"):
        if sum(part["name"] == name for part in partitions) != 1:
            raise ValueError(f"disk must contain exactly one {name}")
    return partitions


def remove_stale_x_artifacts(fs: Path, log=None) -> None:
    # Only the extracted, powered-off private root is passed here.
    for path in ("/tmp/.X11-unix/X0", "/tmp/.X0-lock", "/var/log/Xorg.0.log"):
        if disk.debugfs_exists(fs, path):
            run(["debugfs", "-w", "-R", f"rm {path}", fs],
                stdout=log or subprocess.PIPE, stderr=log or subprocess.STDOUT)
        if disk.debugfs_exists(fs, path):
            raise ValueError(f"could not remove stale private X artifact: {path}")


def prepare(args, out: Path, nonce: str) -> dict:
    sources = (NATIVE_SOURCE, X11_SOURCE, GUEST_SOURCE, Path(__file__).resolve(),
               args.sdk / "bin/reliefos-musl-cc", args.sysroot / "lib/libc.a",
               args.x11_root / "usr/include/X11/Xlib.h", args.x11_root / "usr/lib/libX11.so")
    source_hashes = {str(path): sha256(path) for path in sources}
    cohort = validate_cohort(args.kernel_payload, args.boot_payload, args.kernel)
    before = sha256(args.image)
    if before != args.source_sha256:
        raise ValueError("source image identity mismatch")
    info = json.loads(subprocess.check_output(["qemu-img", "info", "--output=json", str(args.image)], text=True))
    image_format = validate_image_metadata(info, args.image)
    raw = out / "guest-private.raw"
    with (out / "prepare.log").open("w") as log:
        if image_format == "raw":
            run(["cp", "--reflink=auto", "--sparse=always", args.image, raw], stdout=log, stderr=log)
        else:
            run(["qemu-img", "convert", "-f", "vmdk", "-O", "raw", args.image, raw], stdout=log, stderr=log)
        partitions = validate_partitions(raw, disk.gpt_partitions(raw))
        esp_matches = [p for p in partitions if p["name"] == "RELIEFOS_ESP"]
        root_matches = [p for p in partitions if p["name"] == "RELIEFOS_ROOT"]
        if len(esp_matches) != 1 or len(root_matches) != 1:
            raise ValueError("disk must contain one RELIEFOS_ESP and one RELIEFOS_ROOT")
        esp, root = esp_matches[0], root_matches[0]
        layout = {"root_first": root["first"], "root_last": root["last"]}
        fs = disk.extract_root(raw, layout, out)
        check = subprocess.run(["e2fsck", "-p", str(fs)], cwd=ROOT, stdout=log, stderr=log)
        if check.returncode not in (0, 1):
            raise ValueError(f"root filesystem check failed: {check.returncode}")
        remove_stale_x_artifacts(fs, log)
        # A pre-existing same-release tree could contain unlisted modules or
        # boot files. Refuse it rather than leave unverifiable members behind.
        for existing in ("/" + cohort["manifest"]["module_directory"],
                         "/usr/lib/reliefos/kernel/" + cohort["release"]):
            if disk.debugfs_exists(fs, existing):
                raise ValueError(f"source already contains selected release tree: {existing}")
        native, gui = compile_probes(out, args.sysroot, args.sdk, args.x11_root, log)
        fixtures = generate_fixture(out / "fixture")
        guest = GUEST_SOURCE.read_text()
        for key, value in {"NONCE": nonce, "RELEASE": cohort["release"],
                           "MODES": " ".join(args.modes), "REPEAT": str(args.repeat),
                           "JOBS": str(args.jobs)}.items():
            guest = guest.replace("@@" + key + "@@", value)
        if "@@" in guest:
            raise ValueError("unsubstituted guest template")
        script = out / "guest.sh"; script.write_text(guest)
        run(["sh", "-n", script], stdout=log, stderr=log)
        service = out / "service"
        service.write_text("#!/sbin/openrc-run\ndescription=\"SMP private measurement\"\n"
                           "depend() { after localmount; before xdm reliefos-session; }\n"
                           f"start() {{ /bin/sh {GUEST_DIR}/guest.sh; }}\n")
        payload = [(args.kernel_payload / name, "/" + name, 0o644)
                   for name in cohort["manifest"]["files"]]
        bundle = f'usr/lib/reliefos/kernel/{cohort["release"]}/bundle.json'
        payload.append((args.kernel_payload / bundle, "/" + bundle, 0o644))
        payload += [(args.kernel_payload / "kernel-payload.json", GUEST_DIR + "/kernel-payload.json", 0o644),
                    (native, GUEST_DIR + "/smp-native", 0o755),
                    (gui, GUEST_DIR + "/smp-x11", 0o755),
                    (script, GUEST_DIR + "/guest.sh", 0o755), (service, SERVICE, 0o755)]
        payload += [(fixture, GUEST_DIR + "/fixture/" + fixture.name, 0o644) for fixture in fixtures]
        verify_cohort_pins(cohort)
        payload_digests = [cohort["pins"].get(str(origin.absolute()), {}).get("sha256") or sha256(origin)
                           for origin, _, _ in payload]
        commands = []
        for origin, destination, mode in payload:
            disk.add_missing_root_directory(fs, str(Path(destination).parent), commands)
            if disk.debugfs_exists(fs, destination):
                commands.append("rm " + destination)
            commands += [f"write {quote_debugfs(origin)} {destination}",
                         f"set_inode_field {destination} mode 010{mode:04o}"]
        disabled_links = ("/etc/runlevels/default/smp-setup", "/etc/runlevels/default/reliefos-dhcp",
                          "/etc/runlevels/default/reliefos-ntp", "/etc/runlevels/default/xdm",
                          "/etc/runlevels/default/reliefos-session")
        for stale in (*disabled_links, SERVICE_LINK):
            if disk.debugfs_exists(fs, stale): commands.append("rm " + stale)
        commands.append(f"symlink {SERVICE_LINK} {SERVICE}")
        batch = out / "debugfs.commands"; batch.write_text("\n".join(dict.fromkeys(commands)) + "\n")
        run(["debugfs", "-w", "-f", batch, fs], stdout=log, stderr=log)
        readbacks = []
        for index, (origin, destination, _) in enumerate(payload):
            back = out / f"root-readback-{index:03}"
            run(["debugfs", "-R", f"dump {destination} {quote_debugfs(back)}", fs], stdout=log, stderr=log)
            digest = payload_digests[index]
            if sha256(back) != digest:
                raise ValueError(f"root readback mismatch: {destination}")
            readbacks.append({"path": destination, "sha256": digest})
        if any(disk.debugfs_exists(fs, stale) for stale in disabled_links) or not disk.debugfs_exists(fs, SERVICE_LINK):
            raise ValueError("private service installation failed")
        run(["e2fsck", "-fn", fs], stdout=log, stderr=log)
        disk.put_root_back(raw, fs, layout)
        image_arg = f'{raw}@@{int(esp["first"]) * 512}'
        created = set()
        for member in cohort["esp_files"]:
            parent = Path(member).parent
            for depth in range(1, len(parent.parts) + 1):
                dirname = "/".join(parent.parts[:depth])
                if dirname in created: continue
                target = "::/" + dirname
                exists = subprocess.run(["mdir", "-i", image_arg, target], stdout=log, stderr=log)
                if exists.returncode:
                    run(["mmd", "-i", image_arg, target], stdout=log, stderr=log)
                created.add(dirname)
            run(["mcopy", "-o", "-i", image_arg, args.boot_payload / member, "::/" + member],
                stdout=log, stderr=log)
        esp_readbacks = []
        for index, member in enumerate(cohort["esp_files"]):
            back = out / f"esp-readback-{index:03}"
            run(["mcopy", "-i", image_arg, "::/" + member, back], stdout=log, stderr=log)
            digest = cohort["pins"][str((args.boot_payload / member).absolute())]["sha256"]
            if sha256(back) != digest:
                raise ValueError(f"ESP readback mismatch: {member}")
            esp_readbacks.append({"path": member, "sha256": digest})
        # Check a byte-for-byte extracted FAT filesystem without repairing the ESP.
        check_esp(raw, int(esp["first"]), int(esp["last"]), out / "guest-esp.fat", log)
    if sha256(args.image) != before:
        raise ValueError("source image changed during preparation")
    verify_source_hashes(source_hashes)
    verify_cohort_pins(cohort)
    if validate_cohort(args.kernel_payload, args.boot_payload, args.kernel)["pins"] != cohort["pins"]:
        raise ValueError("cohort identity changed during preparation")
    inputs = source_hashes.copy()
    inputs.update({"fixture/" + p.name: sha256(p) for p in fixtures})
    inputs.update({"binary/smp-native": sha256(native), "binary/smp-x11": sha256(gui)})
    record = {"source": str(args.image), "source_sha256": before, "raw": str(raw),
              "raw_sha256": sha256(raw), "release": cohort["release"], "nonce": nonce,
              "kernel_sha256": cohort["kernel_sha256"], "initramfs_sha256": cohort["initramfs_sha256"],
              "module_abi": cohort["module_abi"], "kernel_manifest": cohort["manifest"],
              "cohort_pins": cohort["pins"],
              "root_readbacks": readbacks, "esp_readbacks": esp_readbacks,
              "inputs": inputs, "cpus": args.cpus, "repeat": args.repeat, "jobs": args.jobs,
              "modes": args.modes, "cache_policy": "warm-up compile and encode/decode, then repeat without cache dropping"}
    (out / "prepared.json").write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
    raw.chmod(0o400)
    return record


def refuse_existing_vm(proc_root: Path = Path("/proc")) -> None:
    for process in proc_root.iterdir():
        if not process.name.isdecimal(): continue
        names = []
        try:
            parts = (process / "cmdline").read_bytes().split(b"\0")
            if parts and parts[0]: names.append(Path(os.fsdecode(parts[0])).name)
        except (FileNotFoundError, ProcessLookupError, PermissionError): pass
        try:
            names.append(Path(os.readlink(process / "exe")).name.removesuffix(" (deleted)"))
        except (FileNotFoundError, ProcessLookupError, PermissionError, OSError): pass
        if any(name.startswith("qemu-system-") or name in {"qemu-kvm", "kvm", "vmware-vmx"} for name in names):
            raise RuntimeError(f"existing VM process {process.name}; refusing to start another")


def boot(prepared: dict, args, out: Path) -> dict:
    result = {"passed": False, "phase": "preflight", "cleanup": {"status": "not_started"},
              "timeout_seconds": args.timeout}
    started = time.monotonic()
    initial = None
    raw = None
    serial = out / "serial.log"
    try:
        refuse_existing_vm()
        result["phase"] = "image"
        raw = Path(prepared["raw"])
        initial = sha256(raw)
        result["prepared_sha256"] = initial
        if initial != prepared["raw_sha256"]:
            raise ValueError("prepared image identity changed")
        result["phase"] = "accelerator"
        accelerator = disk.choose_accelerator(args.accelerator)
        result["phase"] = "startup"
        # Keep Unix sockets short and on the worktree parent filesystem.
        with tempfile.TemporaryDirectory(prefix=".smp-qmp-", dir=ROOT.parent) as temporary, \
                (out / "qemu.log").open("w") as log:
            qmp = Path(temporary) / "qmp.sock"
            cpu = "host" if accelerator == "kvm" else "max"
            command = ["qemu-system-x86_64", "-machine", "q35", "-accel", accelerator,
                       "-cpu", cpu, "-m", "4096", "-smp", str(args.cpus), "-display", "none",
                       "-serial", f"file:{serial}", "-vga", "vmware", "-nic", "none",
                       "-drive", f"file={raw},format=raw,if=none,id=osdisk,snapshot=on",
                       "-device", "nvme,drive=osdisk,serial=smp-latency,bootindex=1",
                       "-qmp", f"unix:{qmp},server=on,wait=off", "-no-reboot", "-no-shutdown",
                       "-fw_cfg", "name=opt/org.seabios/pci64,string=0"]
            result["command"] = command
            env = {**os.environ, "TMPDIR": str(ROOT / "out/tmp")}
            process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=log, stderr=log)
            result["pid"] = process.pid
            try:
                result["phase"] = "workload"
                deadline = time.monotonic() + args.timeout
                while time.monotonic() < deadline:
                    text = serial.read_text(errors="replace") if serial.exists() else ""
                    if FATAL.search(text): raise RuntimeError("guest fatal marker")
                    if re.search(r"(?m)^SMP_DONE:" + re.escape(prepared["nonce"]) + r":failures=\d+\r?\n", text):
                        break
                    if process.poll() is not None: raise RuntimeError("QEMU exited before completion")
                    time.sleep(0.25)
                else:
                    raise TimeoutError("guest measurement timed out")
                result["measurements"] = parse_serial(text, prepared["nonce"], prepared["release"],
                                                      args.modes, args.repeat)
            except (Exception, SystemExit) as error:
                result["error"] = repr(error)
            finally:
                try:
                    stop_owned(process, qmp)
                    result["qemu_exit"] = process.returncode
                    if process.returncode != 0:
                        raise RuntimeError(f"owned QEMU exited with status {process.returncode}")
                    result["cleanup"] = {"status": "ok"}
                except (Exception, SystemExit) as error:
                    result["cleanup"] = {"status": "failed", "error": repr(error)}
                    if "error" not in result:
                        result["phase"] = "cleanup"
                        result["error"] = repr(error)
        if "error" not in result:
            result["passed"] = True
            result["phase"] = "complete"
    except (Exception, SystemExit) as error:
        result["error"] = repr(error)
    finally:
        result["elapsed_seconds"] = time.monotonic() - started
        if initial is not None:
            try:
                result["prepared_image_unchanged"] = initial == sha256(raw)
                if not result["prepared_image_unchanged"]:
                    result["passed"] = False
                    if "error" not in result:
                        result["phase"] = "image"
                        result["error"] = "prepared image changed during execution"
            except OSError as error:
                result["prepared_image_unchanged"] = False
                result["passed"] = False
                if "error" not in result:
                    result["phase"] = "image"
                    result["error"] = repr(error)
        (out / "result.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--validate-serial", type=Path, help="validate an existing serial log without starting a VM")
    parser.add_argument("--nonce")
    parser.add_argument("--release")
    parser.add_argument("--image", type=Path, help="source raw or VMDK; never modified")
    parser.add_argument("--source-sha256", help="expected SHA-256 of the source image")
    parser.add_argument("--kernel", type=Path, help="selected kernel.sys")
    parser.add_argument("--kernel-payload", type=Path, help="controller-built root kernel-payload directory")
    parser.add_argument("--boot-payload", type=Path, help="matching controller-built ESP directory")
    parser.add_argument("--sysroot", type=Path)
    parser.add_argument("--sdk", type=Path, default=ROOT / "out/x86_64/release/sdk/reliefos-musl-sdk")
    parser.add_argument("--x11-root", type=Path, default=ROOT / "out/x86_64/release/upstream/x11-development")
    parser.add_argument("--out", type=Path, help="new output directory")
    parser.add_argument("--prepare-only", action="store_true", help="prepare image for QEMU or VMware without starting a VM")
    parser.add_argument("--expect-profiler", choices=("on", "off", "both", "absent"), required=True)
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument("--jobs", type=int, default=4, help="make jobs; 0 means unbounded make -j on the fixed 32-TU fixture")
    parser.add_argument("--timeout", type=float, default=1800)
    parser.add_argument("--accelerator", choices=("auto", "kvm", "tcg"), default="auto")
    args = parser.parse_args()
    if not 1 <= args.repeat <= 10 or not 1 <= args.cpus <= 8 or not 0 <= args.jobs <= 32 or not 0 < args.timeout <= 86400:
        parser.error("invalid repeat, CPU count, job count or timeout")
    args.modes = ["off", "on"] if args.expect_profiler == "both" else [args.expect_profiler]
    if args.validate_serial:
        if not args.nonce or not args.release:
            parser.error("--validate-serial requires --nonce and --release")
        try:
            result = parse_serial(args.validate_serial.read_text(errors="replace"), args.nonce,
                                  args.release, args.modes, args.repeat)
        except (OSError, ValueError) as error:
            print(f"SMP validation failed: {error}", file=sys.stderr)
            return 1
        print(json.dumps(result, sort_keys=True))
        return 0
    required = (args.image, args.source_sha256, args.kernel, args.kernel_payload,
                args.boot_payload, args.sysroot, args.out)
    if any(value is None for value in required) or not re.fullmatch(r"[a-f0-9]{64}", args.source_sha256 or ""):
        parser.error("measurement requires --image, --source-sha256, --kernel, --kernel-payload, --boot-payload, --sysroot and --out")
    for name in ("image", "kernel", "kernel_payload", "boot_payload", "sysroot", "sdk", "x11_root", "out"):
        setattr(args, name, getattr(args, name).resolve())
    args.out.mkdir(parents=True, exist_ok=False)
    try:
        prepared = prepare(args, args.out, uuid.uuid4().hex)
        print(f'PREPARED {prepared["raw"]} release={prepared["release"]}', flush=True)
        if args.prepare_only:
            return 0
        result = boot(prepared, args, args.out)
        if not result["passed"]:
            print(f'SMP measurement failed; see {args.out / "result.json"}', file=sys.stderr)
            return 1
        print(f'PASS measurements={len(result["measurements"]["rows"])}', flush=True)
        return 0
    except Exception as error:
        failure = {"passed": False, "error": repr(error)}
        (args.out / "prepare-result.json").write_text(json.dumps(failure, indent=2) + "\n")
        print(f"SMP preparation failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
