#!/usr/bin/env python3
"""Regression tests for the SMP measurement review; no VM is launched."""
import hashlib
import importlib.util
import json
import os
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from tools import test_smp_latency_qemu_unit as original

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("smp_repair_runner", ROOT / "tools/test_smp_latency_qemu.py")
runner = importlib.util.module_from_spec(spec); spec.loader.exec_module(runner)
TOOLS = ("gcc", "make", "as", "ld", "ffmpeg", "ffprobe")


def version_markers():
    return "".join(f"SMP_VERSION_LINE:{original.NONCE}:{name}:1:{name} version 1.2\n"
                   f"SMP_VERSION_END:{original.NONCE}:{name}:0:1\n" for name in TOOLS)


def cohort_fixture(tmp, release="5.1.2-ko1-aaaaaaaaaaaa", abi="a" * 64, generation="1"):
    root, esp = Path(tmp) / "root", Path(tmp) / "esp"
    def put(base, name, data):
        path = base / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(data); return path
    def sha(data): return hashlib.sha256(data).hexdigest()
    kernel, init = b"kernel", b"initramfs"; kh, ih = sha(kernel), sha(init)
    prefix = f"usr/lib/reliefos/kernel/{release}"; moddir = f"lib/modules/{release}"
    env = original.grub_environment({"reliefos_generation": generation, "reliefos_release": release,
                                     "reliefos_module_abi": abi, "reliefos_kernel_sha256": kh,
                                     "reliefos_initramfs_sha256": ih})
    files = {f"{prefix}/kernel.sys": kernel, f"{prefix}/initramfs.cpio": init,
             f"{prefix}/boot.env": env, f"{moddir}/module.abi": (abi + "\n").encode(),
             f"{moddir}/modules.dep": b"dep\n", "usr/lib/reliefos/kernel/current-release": (release + "\n").encode()}
    files[f"{prefix}/SHA256SUMS"] = "".join(f"{sha(data)}  {name}\n" for name, data in files.items()
                                              if not name.endswith("current-release")).encode()
    for name, data in files.items(): put(root, name, data)
    manifest = {"schema": 2, "release": release, "module_abi": abi, "kernel": f"{prefix}/kernel.sys",
                "initramfs": f"{prefix}/initramfs.cpio", "module_directory": moddir,
                "files": {name: {"sha256": sha(data), "size": len(data)} for name, data in files.items()}}
    manifest_data = json.dumps(manifest).encode(); put(root, "kernel-payload.json", manifest_data); put(root, f"{prefix}/bundle.json", manifest_data)
    version = f"reliefos/versions/{release}/{ih}"
    boot = {"schema": 1, "release": release, "module_abi": abi, "kernel_sha256": kh, "initramfs_sha256": ih}
    for name, data in {"kernel.sys": kernel, "initramfs.cpio": init, "boot.env": env,
                       "boot.json": json.dumps(boot).encode(),
                       "SHA256SUMS": (f"{kh}  kernel.sys\n{ih}  initramfs.cpio\n{sha(env)}  boot.env\n").encode()}.items(): put(esp, f"{version}/{name}", data)
    for slot in ("A", "B"):
        put(esp, f"reliefos/state/kernel-{slot}.env", env)
        put(esp, f"reliefos/state/kernel-{slot}.sha256", f"{sha(env)}  kernel-{slot}.env\n".encode())
    for brand in ("reliefos", "leonos"):
        put(esp, f"{brand}/kernel.sys", kernel); put(esp, f"{brand}/initramfs.cpio", init)
    return root, esp, root / f"{prefix}/kernel.sys"


class ProfileContract(unittest.TestCase):
    def test_stopped_counter_and_histogram_invariants(self):
        valid = original.snapshot().splitlines()
        runner.parse_stats(valid, required=True)
        zero = valid[1].replace("wait_total=2 wait_max=2", "wait_total=0 wait_max=0").replace("wait_hist=0,1,", "wait_hist=1,0,")
        runner.parse_stats([valid[0], zero], required=True)
        bad = [valid[1].replace("contentions=0", "contentions=2"),
               valid[1].replace("wait_total=2", "wait_total=1"),
               valid[1].replace("wait_hist=0,1,", "wait_hist=0,0,"),
               valid[1].replace("hold_hist=0,", "hold_hist=2,"),
               valid[1].replace("irq_hist=0,", "irq_hist=2,"),
               valid[1].replace("acquisitions=1", "acquisitions=18446744073709551616")]
        for row in bad:
            with self.subTest(row=row[:100]), self.assertRaises(ValueError):
                runner.parse_stats([valid[0], row], required=True)
        sat = valid[1].replace("acquisitions=1", "acquisitions=18446744073709551615").replace(
            "wait_hist=0,1,", "wait_hist=0,18446744073709551615,")
        runner.parse_stats([valid[0], sat], required=True)


class CohortIdentityContract(unittest.TestCase):
    def test_grub_environment_tool_timeout_fails_closed(self):
        with mock.patch.object(runner.subprocess, "run", side_effect=subprocess.TimeoutExpired("grub-editenv", 5)) as command:
            with self.assertRaises(ValueError): runner.environment_values(b"anything")
            self.assertEqual(command.call_args.kwargs["timeout"], 5)

    def test_grub_environment_signatures_are_validated_even_with_updated_hashes(self):
        for target in ("version", "A", "B"):
            with self.subTest(target=target), tempfile.TemporaryDirectory() as tmp:
                root, esp, selected = cohort_fixture(tmp)
                runner.validate_cohort(root, esp, selected)
                manifest_path = root / "kernel-payload.json"
                manifest = json.loads(manifest_path.read_text())
                release, ih = manifest["release"], manifest["files"][manifest["initramfs"]]["sha256"]
                prefix = f"usr/lib/reliefos/kernel/{release}"
                version = esp / f"reliefos/versions/{release}/{ih}"
                if target == "version":
                    path = root / prefix / "boot.env"
                    bad = path.read_bytes().replace(b"# GRUB Environment Block", b"# BAD! Environment Block", 1)
                    path.write_bytes(bad); (version / "boot.env").write_bytes(bad)
                    record = manifest["files"][f"{prefix}/boot.env"]
                    record.update(sha256=hashlib.sha256(bad).hexdigest(), size=len(bad))
                    root_sums = root / prefix / "SHA256SUMS"
                    root_sums.write_text("".join(f'{record["sha256"]}  {name}\n' for name, record in manifest["files"].items()
                                                 if name not in {f"{prefix}/SHA256SUMS", "usr/lib/reliefos/kernel/current-release"}))
                    record = manifest["files"][f"{prefix}/SHA256SUMS"]
                    record.update(sha256=hashlib.sha256(root_sums.read_bytes()).hexdigest(), size=root_sums.stat().st_size)
                    content = json.dumps(manifest).encode()
                    manifest_path.write_bytes(content); (root / prefix / "bundle.json").write_bytes(content)
                    (version / "SHA256SUMS").write_text(
                        f'{manifest["files"][manifest["kernel"]]["sha256"]}  kernel.sys\n{ih}  initramfs.cpio\n'
                        f'{hashlib.sha256(bad).hexdigest()}  boot.env\n')
                    bad_path = version / "boot.env"
                else:
                    bad_path = esp / f"reliefos/state/kernel-{target}.env"
                    bad = bad_path.read_bytes().replace(b"# GRUB Environment Block", b"# BAD! Environment Block", 1)
                    bad_path.write_bytes(bad)
                    bad_path.with_suffix(".sha256").write_text(f'{hashlib.sha256(bad).hexdigest()}  {bad_path.name}\n')
                result = subprocess.run(["grub-editenv", str(bad_path), "list"], capture_output=True, timeout=5)
                self.assertNotEqual(result.returncode, 0, "fixture must fail the real GRUB parser")
                with self.assertRaises(ValueError): runner.validate_cohort(root, esp, selected)

    def test_slot_generation_follows_checked_grub_range(self):
        for generation in ("2", "2147483647", "0", "2147483648", "01"):
            with self.subTest(generation=generation), tempfile.TemporaryDirectory() as tmp:
                root, esp, selected = cohort_fixture(tmp)
                for slot in ("A", "B"):
                    path = esp / f"reliefos/state/kernel-{slot}.env"
                    fields = runner.environment_values(path.read_bytes()); fields["reliefos_generation"] = generation
                    data = original.grub_environment(fields)
                    path.write_bytes(data)
                    (path.parent / f"kernel-{slot}.sha256").write_text(f"{hashlib.sha256(data).hexdigest()}  kernel-{slot}.env\n")
                if generation in ("2", "2147483647"):
                    runner.validate_cohort(root, esp, selected)
                else:
                    with self.assertRaises(ValueError): runner.validate_cohort(root, esp, selected)

    def test_grub_release_abi_and_generation_are_enforced(self):
        for release, abi, generation in (("5.1.2-ko1-aaaaaaaaaaaa", "a" * 64, "1"),
                                         ("short", "a" * 64, "1"),
                                         ("5.1.2-ko1-bbbbbbbbbbbb", "a" * 64, "1"),
                                         ("5.1.2-ko1-aaaaaaaaaaaa", "a" * 64, "2"),
                                         ("x" * 31 + "aaaaaaaaaaaa", "a" * 64, "1")):
            with self.subTest(release=release, generation=generation), tempfile.TemporaryDirectory() as tmp:
                args = cohort_fixture(tmp, release, abi, generation)
                if release == "5.1.2-ko1-aaaaaaaaaaaa" and generation == "1": runner.validate_cohort(*args)
                else:
                    with self.assertRaises(ValueError): runner.validate_cohort(*args)

    def test_mutated_cohort_member_after_validation_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root, esp, selected = cohort_fixture(tmp)
            frozen = runner.validate_cohort(root, esp, selected)
            runner.verify_cohort_pins(frozen)
            for path in (selected, esp / "reliefos/state/kernel-A.sha256", root / "kernel-payload.json"):
                frozen = runner.validate_cohort(root, esp, selected)
                old = path.read_bytes(); path.write_bytes(old + b"changed")
                with self.subTest(path=path), self.assertRaises(ValueError): runner.verify_cohort_pins(frozen)
                path.write_bytes(old)


class VersionContract(unittest.TestCase):
    def test_version_output_is_required_unique_complete_and_successful(self):
        text = original.serial(versions=False)
        start = f"SMP_START:{original.NONCE}:{original.RELEASE}\n"
        with_versions = text.replace(start, start + version_markers())
        parsed = runner.parse_serial(with_versions, original.NONCE, original.RELEASE, ["on"], 1)
        self.assertEqual(parsed["versions"]["gcc"]["lines"], ["gcc version 1.2"])
        variants = [text, with_versions.replace("SMP_VERSION_END:abc123:gcc:0:1\n", ""),
                    with_versions.replace("SMP_VERSION_END:abc123:gcc:0:1", "SMP_VERSION_END:abc123:gcc:1:1"),
                    with_versions.replace("SMP_VERSION_LINE:abc123:gcc:1:", "SMP_VERSION_LINE:other:gcc:1:"),
                    with_versions.replace("SMP_VERSION_LINE:abc123:gcc:1:", "SMP_VERSION_LINE:abc123:gcc:2:"),
                    with_versions.replace("SMP_VERSION_END:abc123:gcc:0:1", "SMP_VERSION_END:abc123:gcc:0:2"),
                    with_versions.replace("SMP_VERSION_END:abc123:gcc:0:1", "unexpected\nSMP_VERSION_END:abc123:gcc:0:1"),
                    with_versions.replace("SMP_VERSION_END:abc123:gcc:0:1\n", "SMP_VERSION_END:abc123:gcc:0:1\n" * 2)]
        for variant in variants:
            with self.subTest(variant=variant[:100]), self.assertRaises(ValueError):
                runner.parse_serial(variant, original.NONCE, original.RELEASE, ["on"], 1)


class ConsoleInterleavingContract(unittest.TestCase):
    # Exact first off/idle stats block from baseline-qemu-v2/serial.log.
    REAL_BLOCK = (
        "SMP_STATS_BEGIN:02c60b6b4cee4388aea680c712c9b6fa:off:idle:1\n"
        "[   69.851328] \x1b[38;2;128;144;160m[DEBUG] \x1b[0m[reliefnt] busybox prepared lazy Ring-3 image entry=0x4041470 cr3=0x4381000\n"
        "[   69.853924] \x1b[38;2;128;144;160m[DEBUG] \x1b[0m[reliefnt] exec pid=451 path=/bin/busybox pty=0 committed cr3=0x4381000\n"
        "reliefnt_lock_stats 1 unit=tsc_cycles enabled=0 epoch=2 histogram=log2_cycles irq=gate_masked_window\n"
        "[   69.864438] \x1b[38;2;128;144;160m[DEBUG] \x1b[0m[reliefnt] scheduler task exited pid=451 name=busybox code=0\n"
        "[   69.866720] \x1b[38;2;128;144;160m[DEBUG] \x1b[0m[reliefnt] scheduler wait reaped pid=451 by pid=265\n"
        "SMP_STATS_END:02c60b6b4cee4388aea680c712c9b6fa:off:idle:1\n"
    )
    DEBUG = "[   69.851328] \x1b[38;2;128;144;160m[DEBUG] \x1b[0m[reliefnt] busybox prepared lazy Ring-3 image entry=0x4041470 cr3=0x4381000\n"

    def valid_serial(self):
        text = original.serial().replace(":on:", ":off:")
        empty_header = original.snapshot().splitlines(keepends=True)[0]
        text = text.replace(original.snapshot(), empty_header)
        old = f"SMP_STATS_BEGIN:{original.NONCE}:off:idle:1\n" + empty_header + f"SMP_STATS_END:{original.NONCE}:off:idle:1\n"
        text = text.replace(old, self.REAL_BLOCK.replace("02c60b6b4cee4388aea680c712c9b6fa", original.NONCE))
        for marker in (f"SMP_VERSION_LINE:{original.NONCE}:gcc:1:gcc version 1.2\n",
                       f"SMP_X11_BEGIN:{original.NONCE}:off:idle:1\n",
                       f"SMP_NATIVE_BEGIN:{original.NONCE}:off:native:1\n"):
            text = text.replace(marker, marker + self.DEBUG, 1)
        return text

    def test_real_console_block_and_interleavings_parse(self):
        parsed = runner.parse_serial(self.valid_serial(), original.NONCE, original.RELEASE, ["off"], 1)
        self.assertEqual(len(parsed["rows"]), 4)
        self.assertEqual(parsed["versions"]["gcc"]["lines"], ["gcc version 1.2"])
        self.assertEqual(next(row for row in parsed["rows"] if row["case"] == "idle")["telemetry"]["epoch"], 2)
        plain_info = "[   69.851328] [INFO] [reliefnt] exec completed\n"
        runner.parse_serial(self.valid_serial().replace(self.DEBUG, plain_info), original.NONCE,
                            original.RELEASE, ["off"], 1)

    def test_unknown_truncated_or_error_lines_and_fatal_are_not_hidden(self):
        text = self.valid_serial()
        cases = ["unexpected application output\n", "[   69.851328] [UNKNOWN] [reliefnt] noise\n",
                 "[   69.851328] [ERROR] [reliefnt] read failed\n",
                 "[   69.851328] [DEBUG] [reliefnt] SMP_BROKEN\n", "SMP_STATS_BEG\n",
                 "[   69.851328] \x1b[38;2;128;144;160m[DEBU\n", "kernel panic\n"]
        for bad in cases:
            variant = text.replace(self.DEBUG, bad, 1)
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                runner.parse_serial(variant, original.NONCE, original.RELEASE, ["off"], 1)
        for marker in (f"SMP_STATS_BEGIN:{original.NONCE}:off:idle:1\n",
                       f"SMP_X11_BEGIN:{original.NONCE}:off:idle:1\n",
                       f"SMP_NATIVE_BEGIN:{original.NONCE}:off:native:1\n"):
            for bad in ("unexpected application output\n", "[   69.851328] [ERROR] [reliefnt] failure\n", "SMP_BAD\n"):
                with self.subTest(marker=marker, bad=bad), self.assertRaises(ValueError):
                    runner.parse_serial(text.replace(marker, marker + bad, 1), original.NONCE,
                                        original.RELEASE, ["off"], 1)


class NativeSeverityContract(unittest.TestCase):
    @staticmethod
    def log(level, colored):
        color = "\x1b[38;2;176;128;160m" if colored else ""
        reset = "\x1b[0m" if colored else ""
        return f"[   69.851328] {color}[{level}] {reset}[device] unrecoverable failure\n"

    def insert(self, placement, line):
        text = original.serial()
        if placement == "before": return line + text
        if placement == "after": return text + line
        if placement == "between":
            marker = f"SMP_START:{original.NONCE}:{original.RELEASE}\n"
        else:
            marker = f"SMP_STATS_BEGIN:{original.NONCE}:on:idle:1\n"
        return text.replace(marker, marker + line, 1)

    def test_native_fatal_rejected_everywhere_and_error_only_during_run(self):
        for colored in (False, True):
            for placement in ("before", "between", "inside", "after"):
                with self.subTest(level="FATAL", colored=colored, placement=placement), self.assertRaises(ValueError):
                    runner.parse_serial(self.insert(placement, self.log("FATAL", colored)),
                                        original.NONCE, original.RELEASE, ["on"], 1)
                error = self.insert(placement, self.log("ERROR", colored))
                with self.subTest(level="ERROR", colored=colored, placement=placement):
                    if placement in ("between", "inside"):
                        with self.assertRaises(ValueError):
                            runner.parse_serial(error, original.NONCE, original.RELEASE, ["on"], 1)
                    else:
                        runner.parse_serial(error, original.NONCE, original.RELEASE, ["on"], 1)

    def test_truncated_severity_and_malformed_qa_markers_are_rejected(self):
        for colored in (False, True):
            fatal = self.log("FATAL", colored)
            partial = fatal[:fatal.index("[FATAL]") + len("[FATA")]
            for suffix in (fatal.rstrip("\n"), partial):
                with self.subTest(colored=colored, fatal=suffix), self.assertRaises(ValueError):
                    runner.parse_serial(original.serial() + suffix, original.NONCE, original.RELEASE, ["on"], 1)
            error = self.log("ERROR", colored)
            partial = error[:error.index("[ERROR]") + len("[ERR")]
            for line in (error.rstrip("\n"), partial + "\n", "SMP_BROKEN\n"):
                with self.subTest(colored=colored, error=line), self.assertRaises(ValueError):
                    runner.parse_serial(self.insert("between", line), original.NONCE, original.RELEASE, ["on"], 1)


class GptContract(unittest.TestCase):
    def test_truncated_gpt_entry_array_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp) / "disk.raw"
            contents = bytearray(1536)
            contents[512:520] = b"EFI PART"
            struct.pack_into("<QII", contents, 512 + 72, 2, 5, 128)
            image.write_bytes(contents)
            with self.assertRaises(ValueError): runner.disk.gpt_partitions(image)

    def test_rejects_invalid_overlapping_or_out_of_bounds_partitions(self):
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp) / "disk.raw"; image.write_bytes(b"\0" * 65536)
            good = [{"name": "RELIEFOS_ESP", "first": 2, "last": 20},
                    {"name": "RELIEFOS_ROOT", "first": 21, "last": 100}]
            self.assertEqual(len(runner.validate_partitions(image, good)), 2)
            variants = [[good[0], {**good[1], "first": 50, "last": 40}],
                        [good[0], {**good[1], "first": 20}],
                        [good[0], {**good[1], "last": 128}], good + [good[0]]]
            for parts in variants:
                with self.subTest(parts=parts), self.assertRaises(ValueError):
                    runner.validate_partitions(image, parts)
            with self.assertRaises(ValueError):
                runner.disk.extract_root(image, {"root_first": 4, "root_last": 2}, Path(tmp))


class VmContract(unittest.TestCase):
    def test_common_aliases_and_real_executable_are_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            proc = Path(tmp); entry = proc / "42"; entry.mkdir()
            for name in ("qemu-kvm", "kvm", "qemu-system-x86_64", "vmware-vmx"):
                (entry / "cmdline").write_bytes(("/usr/bin/" + name).encode() + b"\0")
                with self.subTest(name=name), self.assertRaises(RuntimeError): runner.refuse_existing_vm(proc)
            (entry / "cmdline").write_bytes(b"renamed\0")
            (entry / "exe").symlink_to("/usr/libexec/qemu-kvm")
            with self.assertRaises(RuntimeError): runner.refuse_existing_vm(proc)

    def test_boot_preflight_errors_always_write_result_json(self):
        args = SimpleNamespace(timeout=5, accelerator="kvm", cpus=4, modes=["on"], repeat=1)
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp); raw = out / "image.raw"; raw.write_bytes(b"image")
            prepared = {"raw": str(raw), "raw_sha256": hashlib.sha256(b"image").hexdigest(),
                        "nonce": "abc123", "release": "5.1.2-test"}
            with mock.patch.object(runner, "refuse_existing_vm", side_effect=RuntimeError("existing VM")):
                result = runner.boot(prepared, args, out)
                self.assertFalse(result["passed"])
                self.assertEqual(json.loads((out / "result.json").read_text())["phase"], "preflight")
            prepared["raw_sha256"] = "0" * 64
            with mock.patch.object(runner, "refuse_existing_vm"):
                result = runner.boot(prepared, args, out)
                self.assertFalse(result["passed"])
                self.assertEqual(json.loads((out / "result.json").read_text())["phase"], "image")
            prepared["raw_sha256"] = hashlib.sha256(b"image").hexdigest()
            with mock.patch.object(runner, "refuse_existing_vm"), \
                 mock.patch.object(runner.disk, "choose_accelerator", side_effect=SystemExit("KVM unavailable")):
                result = runner.boot(prepared, args, out)
                self.assertFalse(result["passed"])
                self.assertEqual(json.loads((out / "result.json").read_text())["phase"], "accelerator")
            with mock.patch.object(runner, "refuse_existing_vm"):
                result = runner.boot({}, args, out)
                self.assertFalse(result["passed"])
                self.assertEqual(json.loads((out / "result.json").read_text())["phase"], "image")

    def test_qmp_failure_never_waits_without_timeout(self):
        from run_gcc_probe_qemu import qmp_quit
        process = mock.Mock()
        process.wait.side_effect = subprocess.TimeoutExpired("owned", 5)
        with mock.patch("run_gcc_probe_qemu.socket.socket", side_effect=OSError("no socket")):
            with self.assertRaises(subprocess.TimeoutExpired): qmp_quit(Path("missing"), process)
        self.assertTrue(process.wait.call_args_list)
        for call in process.wait.call_args_list:
            self.assertIsNotNone(call.kwargs.get("timeout"))

    def test_startup_workload_and_cleanup_failures_are_reported(self):
        args = SimpleNamespace(timeout=5, accelerator="kvm", cpus=4, modes=["on"], repeat=1)
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp); raw = out / "image.raw"; raw.write_bytes(b"image")
            prepared = {"raw": str(raw), "raw_sha256": hashlib.sha256(b"image").hexdigest(),
                        "nonce": "abc123", "release": "5.1.2-test"}
            with mock.patch.object(runner, "refuse_existing_vm"), \
                 mock.patch.object(runner.disk, "choose_accelerator", return_value="kvm"), \
                 mock.patch.object(runner.subprocess, "Popen", side_effect=OSError("startup failed")):
                result = runner.boot(prepared, args, out)
                self.assertEqual(result["phase"], "startup")
                self.assertEqual(result["cleanup"]["status"], "not_started")
            process = mock.Mock(pid=42, returncode=0)
            process.poll.return_value = 1
            with mock.patch.object(runner, "refuse_existing_vm"), \
                 mock.patch.object(runner.disk, "choose_accelerator", return_value="kvm"), \
                 mock.patch.object(runner.subprocess, "Popen", return_value=process), \
                 mock.patch.object(runner, "stop_owned"):
                result = runner.boot(prepared, args, out)
                self.assertEqual(result["phase"], "workload")
                self.assertEqual(result["cleanup"]["status"], "ok")
                self.assertIn("exited before completion", result["error"])
            (out / "serial.log").write_text(original.serial())
            with mock.patch.object(runner, "refuse_existing_vm"), \
                 mock.patch.object(runner.disk, "choose_accelerator", return_value="kvm"), \
                 mock.patch.object(runner.subprocess, "Popen", return_value=process), \
                 mock.patch.object(runner, "stop_owned", side_effect=subprocess.TimeoutExpired("owned", 5)):
                result = runner.boot(prepared, args, out)
                self.assertEqual(result["phase"], "cleanup")
                self.assertEqual(result["cleanup"]["status"], "failed")
                self.assertFalse(result["passed"])
                self.assertEqual(json.loads((out / "result.json").read_text()), result)


class StaleXContract(unittest.TestCase):
    def test_only_exact_stale_files_are_removed_from_private_ext4(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp); source = tmp / "source.ext4"; clone = tmp / "clone.ext4"
            with source.open("wb") as f: f.truncate(16 * 1024 * 1024)
            subprocess.run(["mke2fs", "-q", "-F", "-t", "ext4", str(source)], check=True)
            data = tmp / "data"; data.write_bytes(b"keep-or-delete")
            cmds = tmp / "setup"; cmds.write_text("mkdir /tmp\nmkdir /tmp/.X11-unix\nmkdir /var\nmkdir /var/log\n" +
                "".join(f'write "{data}" {p}\n' for p in
                        ("/tmp/.X11-unix/X0", "/tmp/.X0-lock", "/var/log/Xorg.0.log", "/tmp/unrelated")) +
                "set_inode_field /tmp/.X11-unix/X0 mode 0140777\n")
            subprocess.run(["debugfs", "-w", "-f", str(cmds), str(source)], check=True, capture_output=True)
            before = hashlib.sha256(source.read_bytes()).hexdigest(); shutil.copyfile(source, clone)
            runner.remove_stale_x_artifacts(clone)
            for path in ("/tmp/.X11-unix/X0", "/tmp/.X0-lock", "/var/log/Xorg.0.log"):
                self.assertFalse(runner.disk.debugfs_exists(clone, path))
                self.assertTrue(runner.disk.debugfs_exists(source, path))
            self.assertTrue(runner.disk.debugfs_exists(clone, "/tmp/unrelated"))
            self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(), before)


if __name__ == "__main__":
    unittest.main()
