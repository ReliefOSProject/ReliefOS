#!/usr/bin/env python3
"""Stage an SDK-probe workload with a standard cohort, then run one owned VM.

The guest script controls its own cases. A zero script status is not an
assertion that any particular swap, OOM, SMP or hardware case was observed.
"""
from __future__ import annotations

import argparse
import fcntl
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile
import time
import uuid

import test_smp_latency_qemu as cohort
import test_vdso_qemu as disk
from test_dual_boot_qemu import boot_evidence

ROOT = Path(__file__).resolve().parents[1]
GUEST = "/usr/lib/reliefos/tests/memory"
SERVICE = "/etc/init.d/reliefos-memory-qa"
SERVICE_LINK = "/etc/runlevels/default/reliefos-memory-qa"
FATAL = re.compile(r"(?i)kernel panic|\[panic\]|bugcheck")


def parse_serial(raw: str, nonce: str, release: str) -> dict:
    """Require the matching completed invocation; report only observed markers."""
    text = raw.replace("\r", "")
    start = f"MEMORY_RUN_START nonce={nonce} release={release}"
    starts = [match.start() for match in re.finditer(r"(?m)^" + re.escape(start) + r"$", text)]
    done = list(re.finditer(r"(?m)^MEMORY_RUN_DONE nonce=" + re.escape(nonce) + r" status=(\d+)$", text))
    if len(starts) != 1 or len(done) != 1 or done[0].start() < starts[0]:
        raise ValueError("missing, duplicated or out-of-order nonced markers")
    body = text[starts[0]:done[0].start()]
    if int(done[0][1]) != 0 or FATAL.search(text):
        raise ValueError("guest script failed or guest fatal marker appeared")
    if re.search(r"(?m)^LIFECYCLE_FAIL\b|^PRESSURE_FAIL\b|^OOM_PROBE_FAIL\b", body):
        raise ValueError("guest probe reported a failure")
    cases = re.findall(r"(?m)^LIFECYCLE_PASS case=([A-Za-z0-9_]+)\b", body)
    skips = re.findall(r"(?m)^LIFECYCLE_SKIP case=([A-Za-z0-9_]+) reason=([A-Za-z0-9_]+)$", body)
    observed = re.findall(r"(?m)^PRESSURE_MINCORE pages=(\d+) resident=(\d+) nonresident=(\d+)$", body)
    return {"script_status": 0, "lifecycle_cases": cases,
            "lifecycle_skips": [{"case": name, "reason": reason} for name, reason in skips],
            "pressure_mincore": [{"pages": int(a), "resident": int(b), "nonresident": int(c)}
                                 for a, b, c in observed],
            "pressure_pass_marker": bool(re.search(r"(?m)^PRESSURE_PASS$", body)),
            "oom_pass_marker": bool(re.search(r"(?m)^OOM_PROBE_PASS\b", body))}


def run(command, log):
    return subprocess.run([str(value) for value in command], cwd=ROOT,
                          stdout=log, stderr=log, check=True)


def prepare(args) -> dict:
    """Inject readback-checked inputs into a private copy; never write source."""
    output = args.output.resolve()
    source = args.source.resolve(strict=True)
    source_sha = cohort.sha256(source)
    if source_sha != args.source_sha256:
        raise ValueError("source image identity mismatch")
    selected = cohort.validate_cohort(args.root_payload, args.boot_payload, args.kernel)
    nonce = uuid.uuid4().hex
    script = output / "guest.sh"
    script.write_text(f'''#!/bin/sh
exec >/dev/ttyS0 2>&1
echo "MEMORY_RUN_START nonce={nonce} release=$(uname -r)"
if test "$(uname -s)" != ReliefNT || test "$(uname -r)" != '{selected["release"]}'; then
    echo 'MEMORY_RUN_DONE nonce={nonce} status=125'; exit 125
fi
MEMORY_QA_NONCE='{nonce}' /bin/sh {GUEST}/workload.sh
rc=$?
echo "MEMORY_RUN_DONE nonce={nonce} status=$rc"
exit "$rc"
''')
    service = output / "service"
    service.write_text('#!/sbin/openrc-run\ndescription="Private memory QA"\n'
                       'depend() { after localmount; before xdm reliefos-session; }\n'
                       f'start() {{ /bin/sh {GUEST}/guest.sh; }}\n')
    payload = [(args.root_payload / name, "/" + name, 0o644)
               for name in selected["manifest"]["files"]]
    bundle = f'usr/lib/reliefos/kernel/{selected["release"]}/bundle.json'
    payload += [(args.root_payload / bundle, "/" + bundle, 0o644),
                (script, GUEST + "/guest.sh", 0o755),
                (service, SERVICE, 0o755),
                (args.workload, GUEST + "/workload.sh", 0o755),
                (args.lifecycle, GUEST + "/lifecycle", 0o755),
                (args.pressure, GUEST + "/pressure", 0o755),
                (args.oom, GUEST + "/oom", 0o755)]
    payload_sha = [cohort.sha256(origin) for origin, _, _ in payload]
    disabled = ("/etc/runlevels/default/smp-setup", "/etc/runlevels/default/smp-measure",
                "/etc/runlevels/default/memory-layout-qa",
                "/etc/runlevels/default/reliefos-dhcp", "/etc/runlevels/default/reliefos-ntp",
                "/etc/runlevels/default/xdm", "/etc/runlevels/default/reliefos-session")
    raw = output / "prepared.raw"
    with (output / "prepare.log").open("w") as log:
        info = json.loads(subprocess.check_output(["qemu-img", "info", "--output=json", str(source)], text=True))
        kind = cohort.validate_image_metadata(info, source)
        if kind == "raw":
            run(["cp", "--reflink=auto", "--sparse=always", source, raw], log)
        else:
            run(["qemu-img", "convert", "-f", kind, "-O", "raw", source, raw], log)
        raw.chmod(0o600)
        parts = cohort.validate_partitions(raw, disk.gpt_partitions(raw))
        esp = next(part for part in parts if part["name"] == "RELIEFOS_ESP")
        root = next(part for part in parts if part["name"] == "RELIEFOS_ROOT")
        layout = {"root_first": root["first"], "root_last": root["last"]}
        fs = disk.extract_root(raw, layout, output)
        check = subprocess.run(["e2fsck", "-p", str(fs)], stdout=log, stderr=log)
        if check.returncode not in (0, 1):
            raise ValueError(f"private root fsck failed: {check.returncode}")
        cohort.remove_stale_x_artifacts(fs, log)
        for existing in ("/" + selected["manifest"]["module_directory"],
                         "/usr/lib/reliefos/kernel/" + selected["release"]):
            if disk.debugfs_exists(fs, existing):
                raise ValueError(f"source already contains selected release tree: {existing}")
        commands = []
        for origin, destination, mode in payload:
            disk.add_missing_root_directory(fs, str(Path(destination).parent), commands)
            if disk.debugfs_exists(fs, destination):
                commands.append("rm " + destination)
            commands += [f"write {cohort.quote_debugfs(origin)} {destination}",
                         f"set_inode_field {destination} mode 010{mode:04o}"]
        for stale in (*disabled, SERVICE_LINK):
            if disk.debugfs_exists(fs, stale): commands.append("rm " + stale)
        commands.append(f"symlink {SERVICE_LINK} {SERVICE}")
        batch = output / "debugfs.commands"
        batch.write_text("\n".join(dict.fromkeys(commands)) + "\n")
        run(["debugfs", "-w", "-f", batch, fs], log)
        readback = []
        for i, (_, destination, _) in enumerate(payload):
            back = output / f"root-readback-{i:03}"
            run(["debugfs", "-R", f"dump {destination} {cohort.quote_debugfs(back)}", fs], log)
            if cohort.sha256(back) != payload_sha[i]:
                raise ValueError(f"root readback mismatch: {destination}")
            readback.append({"path": destination, "sha256": payload_sha[i]})
        if any(disk.debugfs_exists(fs, stale) for stale in disabled) or not disk.debugfs_exists(fs, SERVICE_LINK):
            raise ValueError("private runlevel setup failed")
        run(["e2fsck", "-fn", fs], log)
        disk.put_root_back(raw, fs, layout)
        image_arg = f"{raw}@@{int(esp['first']) * 512}"
        created = set()
        for member in selected["esp_files"]:
            parent = Path(member).parent
            for depth in range(1, len(parent.parts) + 1):
                directory = "/".join(parent.parts[:depth])
                if directory in created: continue
                if subprocess.run(["mdir", "-i", image_arg, "::/" + directory], stdout=log, stderr=log).returncode:
                    run(["mmd", "-i", image_arg, "::/" + directory], log)
                created.add(directory)
            run(["mcopy", "-o", "-i", image_arg, args.boot_payload / member, "::/" + member], log)
        esp_readback = []
        for i, member in enumerate(selected["esp_files"]):
            back = output / f"esp-readback-{i:03}"
            run(["mcopy", "-i", image_arg, "::/" + member, back], log)
            expected = selected["pins"][str((args.boot_payload / member).absolute())]["sha256"]
            if cohort.sha256(back) != expected:
                raise ValueError(f"ESP readback mismatch: {member}")
            esp_readback.append({"path": member, "sha256": expected})
        cohort.check_esp(raw, esp["first"], esp["last"], output / "esp.fat", log)
    cohort.verify_cohort_pins(selected)
    if [cohort.sha256(origin) for origin, _, _ in payload] != payload_sha or cohort.sha256(source) != source_sha:
        raise ValueError("source or probe input changed during preparation")
    raw.chmod(0o400)
    record = {"raw": str(raw), "raw_sha256": cohort.sha256(raw),
              "source": str(source), "source_sha256": source_sha,
              "nonce": nonce, "release": selected["release"], "module_abi": selected["module_abi"],
              "cohort_pins": selected["pins"], "root_readbacks": readback, "esp_readbacks": esp_readback}
    (output / "prepared.json").write_text(json.dumps(record, indent=2) + "\n")
    return record


def socket_directory(path: Path) -> Path:
    path = path.resolve(strict=True)
    if not path.is_dir(): raise ValueError("QMP directory must exist")
    if any(char in str(path) for char in (",", "\n", "\r")):
        raise ValueError("QMP directory contains unsupported QEMU option characters")
    fs = subprocess.check_output(["stat", "-f", "-c", "%T", str(path)], text=True).strip()
    if fs in {"tmpfs", "ramfs"}: raise ValueError("QMP directory must not be on tmpfs or ramfs")
    return path


def boot(args, record: dict) -> dict:
    """Run at most one owned VM, bound RSS and retain cleanup/observations."""
    qmp_root = socket_directory(args.qmp_dir)
    flags = os.O_RDWR | os.O_CREAT | getattr(os, "O_NOFOLLOW", 0)
    lock_fd = os.open(qmp_root / ".reliefos-memory-qa.lock", flags, 0o600)
    try:
        if not stat.S_ISREG(os.fstat(lock_fd).st_mode): raise ValueError("invalid QMP lock")
        fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        cohort.refuse_existing_vm()
        return boot_locked(args, record, qmp_root)
    finally:
        os.close(lock_fd)


def boot_locked(args, record: dict, qmp_root: Path) -> dict:
    output = args.output.resolve()
    prepared = Path(record["raw"])
    if cohort.sha256(prepared) != record["raw_sha256"]:
        raise ValueError("prepared image identity mismatch")
    cohort.verify_cohort_pins({"pins": record["cohort_pins"]})
    working = output / "working.raw"
    subprocess.run(["cp", "--reflink=auto", "--sparse=always", str(prepared), str(working)], check=True)
    working.chmod(0o600)
    serial = output / "serial.log"
    result = {"script_completed": False, "cleanup": "not_started", "nonce": record["nonce"],
              "release": record["release"], "memory_mib": args.memory_mib,
              "cpus": args.cpus, "firmware": args.firmware, "peak_rss_kib": 0}
    started = time.monotonic()
    try:
        with tempfile.TemporaryDirectory(prefix=".memory-qmp-", dir=qmp_root) as temporary:
            qmp = Path(temporary) / "qmp.sock"
            if len(os.fsencode(qmp)) >= 104: raise ValueError("QMP path too long")
            accel = disk.choose_accelerator(args.accelerator)
            command = ["qemu-system-x86_64", "-machine", "q35", "-accel", accel,
                       "-cpu", "host" if accel == "kvm" else "max", "-m", str(args.memory_mib),
                       "-smp", str(args.cpus), "-display", "none", "-serial", f"file:{serial}",
                       "-qmp", f"unix:{qmp},server=on,wait=off", "-nic", "none", "-no-reboot", "-no-shutdown",
                       "-drive", f"file={working},if=none,id=disk0,format=raw",
                       "-device", "ide-hd,drive=disk0,bus=ide.0,bootindex=1"]
            if args.firmware == "uefi": command += ["-bios", str(args.firmware_file)]
            result["command"] = command
            env = {**os.environ, "TMPDIR": str(ROOT / "out/tmp")}
            with (output / "qemu.log").open("w") as log:
                process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=log, stderr=log)
                result["pid"] = process.pid
                try:
                    deadline = time.monotonic() + args.timeout
                    while time.monotonic() < deadline:
                        if process.poll() is not None: raise RuntimeError("QEMU exited before guest completion")
                        status = Path(f"/proc/{process.pid}/status").read_text()
                        rss = re.search(r"(?m)^VmRSS:\s+(\d+)\s+kB$", status)
                        if rss:
                            result["peak_rss_kib"] = max(result["peak_rss_kib"], int(rss[1]))
                            if int(rss[1]) > args.rss_limit_mib * 1024: raise RuntimeError("QEMU RSS limit exceeded")
                        text = serial.read_text(errors="replace") if serial.exists() else ""
                        if FATAL.search(text): raise RuntimeError("guest fatal marker")
                        if re.search(r"(?m)^MEMORY_RUN_DONE nonce=" + re.escape(record["nonce"]) + r" status=\d+\r?$", text):
                            result["observations"] = parse_serial(text, record["nonce"], record["release"])
                            if not boot_evidence(text, args.firmware, record["release"], cpus=args.cpus):
                                raise ValueError("missing matching boot/release/CPU evidence")
                            result["script_completed"] = True
                            break
                        time.sleep(.2)
                    else: raise TimeoutError("guest workload timed out")
                finally:
                    try:
                        cohort.stop_owned(process, qmp)
                        result["qemu_exit"] = process.returncode
                        result["cleanup"] = "ok"
                    except Exception as error:
                        result["cleanup"] = repr(error)
                        result["script_completed"] = False
    except Exception as error:
        result["error"] = repr(error)
        result["script_completed"] = False
    finally:
        result["elapsed_seconds"] = round(time.monotonic() - started, 3)
        for name, path, expected in (("prepared_unchanged", prepared, record["raw_sha256"]),
                                     ("source_unchanged", Path(record["source"]), record["source_sha256"])):
            try:
                result[name] = cohort.sha256(path) == expected
            except OSError as error:
                result[name] = False
                result.setdefault("error", repr(error))
        if not result["prepared_unchanged"] or not result["source_unchanged"]:
            result["script_completed"] = False
        (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source", "root-payload", "boot-payload", "kernel", "lifecycle", "pressure", "oom", "workload", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--source-sha256", required=True)
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--qmp-dir", type=Path, help="existing non-tmpfs directory for owned QMP socket")
    parser.add_argument("--firmware", choices=("bios", "uefi"), default="bios")
    parser.add_argument("--firmware-file", type=Path, default=Path("/usr/share/edk2/x64/OVMF.4m.fd"))
    parser.add_argument("--accelerator", choices=("auto", "kvm", "tcg"), default="auto")
    parser.add_argument("--memory-mib", type=int, default=256)
    parser.add_argument("--cpus", type=int, default=2)
    parser.add_argument("--rss-limit-mib", type=int, default=3072)
    parser.add_argument("--timeout", type=int, default=600)
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9a-f]{64}", args.source_sha256) or not 128 <= args.memory_mib <= 65536 or \
            not 1 <= args.cpus <= 8 or not 128 <= args.rss_limit_mib <= 65536 or not 1 <= args.timeout <= 7200:
        parser.error("invalid hash or bounded VM parameters")
    if not args.prepare_only and args.qmp_dir is None: parser.error("--qmp-dir is required for boot")
    for name in ("source", "root_payload", "boot_payload", "kernel", "lifecycle", "pressure", "oom", "workload"):
        setattr(args, name, getattr(args, name).resolve(strict=True))
    args.output = args.output.resolve()
    if any(char in str(args.output) for char in (",", "\n", "\r")):
        parser.error("output path contains unsupported QEMU option characters")
    try:
        args.output.mkdir(parents=True, exist_ok=False)
    except OSError as error:
        print(f"cannot create new output directory: {error}", file=sys.stderr)
        return 1
    phase = "prepare"
    try:
        record = prepare(args)
        print(f'PREPARED {args.output / "prepared.json"} nonce={record["nonce"]}', flush=True)
        if args.prepare_only: return 0
        phase = "boot"
        result = boot(args, record)
        print(json.dumps(result), flush=True)
        return 0 if result["script_completed"] else 1
    except (Exception, SystemExit) as error:
        failure = {"script_completed": False, "phase": phase, "error": repr(error),
                   "cleanup": "not_started"}
        path = args.output / ("prepare-result.json" if phase == "prepare" else "result.json")
        if not path.exists(): path.write_text(json.dumps(failure, indent=2) + "\n")
        print(f"memory QA failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
