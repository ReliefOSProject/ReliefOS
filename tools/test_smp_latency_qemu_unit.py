#!/usr/bin/env python3
"""Host-only contract tests for SMP measurement output; no VM is started."""
import hashlib
import importlib.util
import json
import re
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "tools/test_smp_latency_qemu.py"
NONCE = "abc123"
RELEASE = "5.1.2-test"
CASES = ("idle", "compile", "ffmpeg", "native")


def grub_environment(fields):
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "boot.env"
        subprocess.run(["grub-editenv", str(path), "create"], check=True, timeout=5)
        subprocess.run(["grub-editenv", str(path), "set", *(f"{key}={value}" for key, value in fields.items())],
                       check=True, timeout=5)
        return path.read_bytes()


def snapshot():
    bins = ",".join(["0", "1"] + ["0"] * 30)
    return ("reliefnt_lock_stats 1 unit=tsc_cycles enabled=0 epoch=1 "
            "histogram=log2_cycles irq=gate_masked_window\n"
            "cpu=0 scope=fault acquisitions=1 contentions=0 wait_total=2 "
            "wait_max=2 hold_total=0 hold_max=0 irq_total=0 irq_max=0 "
            f"clock_errors=0 wait_hist={bins} hold_hist=" + ",".join(["0"] * 32) +
            " irq_hist=" + ",".join(["0"] * 32) + "\n")


def serial(versions=True):
    parts = [f"SMP_START:{NONCE}:{RELEASE}\n"]
    if versions:
        for name in ("gcc", "make", "as", "ld", "ffmpeg", "ffprobe"):
            parts += [f"SMP_VERSION_LINE:{NONCE}:{name}:1:{name} version 1.2\n",
                      f"SMP_VERSION_END:{NONCE}:{name}:0:1\n"]
    for case in CASES:
        key = f"{NONCE}:on:{case}:1"
        parts += [f"SMP_STATS_BEGIN:{key}\n", snapshot(), f"SMP_STATS_END:{key}\n",
                  f"SMP_X11_BEGIN:{key}\n",
                  "X11 count=20 p50_ns=10 p95_ns=19 p99_ns=20 max_ns=20 wm=1 pixels=1\n",
                  *(f"X11_SAMPLE ns={n}\n" for n in range(1, 21)), f"SMP_X11_END:{key}\n"]
        if case == "native":
            parts += [f"SMP_NATIVE_BEGIN:{key}\n",
                      "NATIVE_PATTERN page0=12319 page1=12318 page256=12575\n",
                      *(f"NATIVE kind={kind} count=2048 p50_ns=1 p95_ns=2 p99_ns=3 max_ns=4\n"
                        for kind in ("demand", "cow", "shared", "filemap", "syscall")),
                      f"SMP_NATIVE_END:{key}\n"]
        parts.append(f"SMP_ROW:{key}:elapsed_ns=1000000:work=0:check=0:gui=0:gui_samples=20\n")
    parts.append(f"SMP_DONE:{NONCE}:failures=0\n")
    return "".join(parts)


class SerialContract(unittest.TestCase):
    def validate(self, text, mode="on"):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "serial.log"
            path.write_text(text)
            return subprocess.run([sys.executable, str(RUNNER), "--validate-serial", str(path),
                                   "--nonce", NONCE, "--release", RELEASE,
                                   "--expect-profiler", mode, "--repeat", "1"],
                                  text=True, capture_output=True)

    def test_valid_rows_and_partial_hold_histograms_are_accepted(self):
        result = self.validate(serial())
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = json.loads(result.stdout)["rows"]
        self.assertEqual(len(rows), 4)
        self.assertEqual(rows[0]["elapsed_ns"], 1000000)

    def test_incomplete_duplicate_failed_or_unbound_rows_fail(self):
        good = serial()
        row = f"SMP_ROW:{NONCE}:on:idle:1:elapsed_ns=1000000:work=0:check=0:gui=0:gui_samples=20\n"
        variants = (good.replace(row, ""), good.replace(row, row + row),
                    good.replace("work=0", "work=1", 1),
                    good.replace("gui_samples=20", "gui_samples=0", 1),
                    good.replace(f"SMP_ROW:{NONCE}", "SMP_ROW:other", 1),
                    good[:-1], good + "[PANIC] failure\n")
        for variant in variants:
            with self.subTest(variant=variant[:100]):
                self.assertNotEqual(self.validate(variant).returncode, 0)

    def test_missing_or_clock_invalid_telemetry_fails(self):
        good = serial()
        for variant in (good.replace(snapshot(), "", 1),
                        good.replace("clock_errors=0", "clock_errors=1", 1),
                        good.replace("unit=tsc_cycles", "unit=nanoseconds", 1),
                        good.replace("enabled=0", "enabled=1", 1)):
            with self.subTest(variant=variant[:100]):
                self.assertNotEqual(self.validate(variant).returncode, 0)

    def test_missing_or_inconsistent_gui_distribution_fails(self):
        good = serial()
        for variant in (good.replace("X11_SAMPLE ns=20\n", "", 1),
                        good.replace("p95_ns=19", "p95_ns=18", 1),
                        good.replace("pixels=1", "pixels=0", 1)):
            with self.subTest(variant=variant[:100]):
                self.assertNotEqual(self.validate(variant).returncode, 0)

    def test_sampling_off_requires_disabled_empty_snapshot_and_absent_is_explicit(self):
        off = serial().replace(":on:", ":off:")
        self.assertNotEqual(self.validate(off, "off").returncode, 0)
        zero = snapshot().splitlines(keepends=True)[0]
        off = off.replace(snapshot(), zero)
        self.assertEqual(self.validate(off, "off").returncode, 0)
        absent = re.sub(r"SMP_STATS_BEGIN:abc123:off:([a-z]+):1\n.*?SMP_STATS_END:abc123:off:\1:1\n",
                        lambda m: f"SMP_ABSENT:abc123:absent:{m[1]}:1\n", off, flags=re.S)
        absent = absent.replace(":off:", ":absent:")
        self.assertEqual(self.validate(absent, "absent").returncode, 0)
        self.assertNotEqual(self.validate(absent.replace("SMP_ABSENT:abc123:absent:idle:1\n", ""), "absent").returncode, 0)


class CohortContract(unittest.TestCase):
    def test_kernel_initramfs_module_and_slot_corruption_are_rejected(self):
        spec = importlib.util.spec_from_file_location("smp_runner", RUNNER)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        self.assertTrue(hasattr(module, "validate_cohort"), "cohort validation is missing")
        with tempfile.TemporaryDirectory() as tmp:
            root, esp = Path(tmp) / "root", Path(tmp) / "esp"
            release, abi = "5.1.2-ko1-aaaaaaaaaaaa", "a" * 64
            def put(base, name, data):
                path = base / name; path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
                return path
            def sha(data): return hashlib.sha256(data).hexdigest()
            kernel, init = b"selected kernel", b"selected initramfs"
            ksha, isha = sha(kernel), sha(init)
            prefix = f"usr/lib/reliefos/kernel/{release}"
            moddir = f"lib/modules/{release}"
            env = grub_environment({"reliefos_generation": "1", "reliefos_release": release,
                                    "reliefos_module_abi": abi, "reliefos_kernel_sha256": ksha,
                                    "reliefos_initramfs_sha256": isha})
            files = {f"{prefix}/kernel.sys": kernel, f"{prefix}/initramfs.cpio": init,
                     f"{prefix}/boot.env": env, f"{moddir}/module.abi": (abi + "\n").encode(),
                     f"{moddir}/modules.dep": b"module dependency data\n",
                     "usr/lib/reliefos/kernel/current-release": (release + "\n").encode()}
            sums = "".join(f"{sha(data)}  {name}\n" for name, data in files.items()
                           if not name.endswith("current-release")).encode()
            files[f"{prefix}/SHA256SUMS"] = sums
            for name, data in files.items(): put(root, name, data)
            manifest = {"schema": 2, "release": release, "module_abi": abi,
                        "kernel": f"{prefix}/kernel.sys", "initramfs": f"{prefix}/initramfs.cpio",
                        "module_directory": moddir,
                        "files": {name: {"sha256": sha(data), "size": len(data)} for name, data in files.items()}}
            manifest_bytes = json.dumps(manifest).encode()
            put(root, "kernel-payload.json", manifest_bytes)
            put(root, f"{prefix}/bundle.json", manifest_bytes)
            version = f"reliefos/versions/{release}/{isha}"
            boot_json = {"schema": 1, "release": release, "module_abi": abi,
                         "kernel_sha256": ksha, "initramfs_sha256": isha}
            for name, data in {"kernel.sys": kernel, "initramfs.cpio": init, "boot.env": env,
                               "boot.json": json.dumps(boot_json).encode(),
                               "SHA256SUMS": (f"{ksha}  kernel.sys\n{isha}  initramfs.cpio\n"
                                              f"{sha(env)}  boot.env\n").encode()}.items():
                put(esp, f"{version}/{name}", data)
            for slot in ("A", "B"):
                put(esp, f"reliefos/state/kernel-{slot}.env", env)
                put(esp, f"reliefos/state/kernel-{slot}.sha256", f"{sha(env)}  kernel-{slot}.env\n".encode())
            for brand in ("reliefos", "leonos"):
                put(esp, f"{brand}/kernel.sys", kernel)
                put(esp, f"{brand}/initramfs.cpio", init)
            selected = root / f"{prefix}/kernel.sys"
            self.assertEqual(module.validate_cohort(root, esp, selected)["release"], release)
            for target in (root / f"{prefix}/bundle.json", root / f"{moddir}/modules.dep", esp / f"{version}/initramfs.cpio",
                           esp / "reliefos/state/kernel-A.env", selected):
                original = target.read_bytes(); target.write_bytes(original + b"broken")
                with self.subTest(target=target), self.assertRaises(ValueError):
                    module.validate_cohort(root, esp, selected)
                target.write_bytes(original)
            extra = put(root, f"{moddir}/unlisted.ko", b"unexpected module")
            with self.assertRaises(ValueError):
                module.validate_cohort(root, esp, selected)
            extra.unlink()


class FixtureContract(unittest.TestCase):
    def test_generated_make_fixture_compiles_and_checks_exact_result(self):
        spec = importlib.util.spec_from_file_location("smp_runner_fixture", RUNNER)
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        self.assertTrue(hasattr(module, "generate_fixture"), "fixture generator is missing")
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            module.generate_fixture(directory)
            self.assertEqual(len(list(directory.glob("unit*.c"))), 32)
            result = subprocess.run(["make", "-j4"], cwd=directory, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(directory / "fixture")], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, "660e5ee6e1d4b501\n")


class CleanupContract(unittest.TestCase):
    def test_unreachable_qmp_stops_and_reaps_only_the_owned_child(self):
        spec = importlib.util.spec_from_file_location("smp_runner_cleanup", RUNNER)
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        self.assertTrue(hasattr(module, "stop_owned"), "owned cleanup is missing")
        child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])
        unrelated = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])
        try:
            module.stop_owned(child, Path("/nonexistent/smp-test-qmp"))
            self.assertIsNotNone(child.poll())
            self.assertIsNone(unrelated.poll())
        finally:
            for process in (child, unrelated):
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=5)

    def test_existing_vm_process_is_refused_and_not_stopped(self):
        spec = importlib.util.spec_from_file_location("smp_runner_refusal", RUNNER)
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        process = subprocess.Popen(["bash", "-c", "exec -a qemu-system-fake sleep 30"])
        try:
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if Path(f"/proc/{process.pid}/cmdline").read_bytes().startswith(b"qemu-system-fake\0"):
                    break
                time.sleep(0.01)
            else:
                self.fail("fake process did not start")
            with self.assertRaises(RuntimeError): module.refuse_existing_vm()
            self.assertIsNone(process.poll())
        finally:
            if process.poll() is None: process.terminate()
            process.wait(timeout=5)


class EspFilesystemContract(unittest.TestCase):
    def test_invalid_fat_boot_sector_fails_offline_validation(self):
        spec = importlib.util.spec_from_file_location("smp_runner_fat", RUNNER)
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        self.assertTrue(hasattr(module, "check_esp"), "FAT validation is missing")
        with tempfile.TemporaryDirectory() as tmp:
            raw = Path(tmp) / "disk.raw"
            with raw.open("wb") as file: file.truncate(64 * 1024 * 1024)
            subprocess.run(["mkfs.fat", "-F", "32", str(raw)], check=True, capture_output=True)
            module.check_esp(raw, 0, 64 * 1024 * 1024 // 512 - 1, Path(tmp) / "esp.fat")
            with raw.open("r+b") as file: file.seek(510); file.write(b"\0\0")
            with self.assertRaises(ValueError):
                module.check_esp(raw, 0, 64 * 1024 * 1024 // 512 - 1, Path(tmp) / "bad.fat")


class ImageIdentityContract(unittest.TestCase):
    def test_rejects_vmdk_external_extents_or_backing_images(self):
        spec = importlib.util.spec_from_file_location("smp_runner_image", RUNNER)
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        self.assertTrue(hasattr(module, "validate_image_metadata"), "image metadata validation is missing")
        image = Path("/test/guest.vmdk")
        info = {"format": "vmdk", "format-specific": {"data": {
            "extents": [{"filename": str(image)}]}}}
        self.assertEqual(module.validate_image_metadata(info, image), "vmdk")
        split = {"format": "vmdk", "format-specific": {"data": {
            "extents": [{"filename": "/test/external.vmdk"}]}}}
        backing = {**info, "backing-filename": "older.vmdk"}
        for bad in (split, backing):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                module.validate_image_metadata(bad, image)

    def test_changed_source_during_preparation_is_rejected(self):
        spec = importlib.util.spec_from_file_location("smp_runner_freeze", RUNNER)
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        self.assertTrue(hasattr(module, "verify_source_hashes"), "source race check is missing")
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "probe.c"; path.write_text("old\n")
            frozen = {str(path): hashlib.sha256(path.read_bytes()).hexdigest()}
            module.verify_source_hashes(frozen)
            path.write_text("new\n")
            with self.assertRaises(ValueError): module.verify_source_hashes(frozen)

if __name__ == "__main__":
    unittest.main()
