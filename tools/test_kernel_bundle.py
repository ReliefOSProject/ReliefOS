#!/usr/bin/env python3
"""Run the actual runtime bundle validator against native production payloads."""
import hashlib
import json
import os
import re
import stat
import shutil
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock
import test_kernel_modules_stage as fixtures

ROOT = Path(__file__).resolve().parents[1]


def inode_key(path):
    info = path.stat()
    return str(info.st_dev) + "-" + str(info.st_ino)


def snapshot_initial_tree(root, durable):
    """The existing accepted ESP starts durable; later changes need fsync."""
    durable.mkdir()
    for path in (root, *root.rglob("*")):
        if path.is_dir():
            data = b"D\n" + b"".join((str(child.stat().st_mode) + "\t" + inode_key(child) + "\t" + child.name + "\n").encode()
                                      for child in sorted(path.iterdir()))
        else:
            assert path.is_file() and not path.is_symlink()
            data = b"F\n" + path.read_bytes()
        (durable / inode_key(path)).write_bytes(data)
    return inode_key(root)


def restore_durable_tree(durable, root_key, recovered):
    """Lose every unsynced inode byte and parent-directory entry change.

    The inode graph models successful file/dir fsync ordering, not arbitrary
    FAT corruption or a device losing writes whose sync already succeeded.
    An allocated but unsynced new inode is recovered empty.
    """
    recovered.mkdir()
    seen = set()

    def visit(key, directory):
        assert key not in seen
        seen.add(key)
        node = durable / key
        data = node.read_bytes() if node.exists() else b"D\n"
        assert data.startswith(b"D\n")
        for line in data[2:].decode().splitlines():
            mode, child_key, name = line.split("\t")
            assert name not in (".", "..") and "/" not in name
            child = directory / name
            if stat.S_ISDIR(int(mode)):
                child.mkdir()
                visit(child_key, child)
            else:
                assert stat.S_ISREG(int(mode))
                node = durable / child_key
                data = node.read_bytes() if node.exists() else b"F\n"
                assert data.startswith(b"F\n")
                child.write_bytes(data[2:])
    visit(root_key, recovered)


class BundleGuestGuard(unittest.TestCase):
    def test_actual_static_recovery_refuses_host_root(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-static-recovery-guard-") as directory:
            trace = Path(directory) / "mutations.trace"
            result = subprocess.run(["strace", "-o", str(trace), "-e",
                "trace=mount,unlink,unlinkat,rename,renameat,mkdir,mkdirat,creat,truncate,ftruncate,pwrite64,fsync,open,openat",
                str(fixtures.RECOVERY), "--recover-running", "/"], text=True, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 77, result.stdout + result.stderr)
            self.assertIn("refusing mutations", result.stderr)
            self.assertEqual(trace.read_text().strip(), "+++ exited with 77 +++")

    def test_real_host_refused_before_filesystem_mutations(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-bundle-guest-guard-") as directory:
            work = Path(directory)
            binary, trace = work / "probe", work / "mutations.trace"
            subprocess.run(["cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "userland/runtime/include"),
                            str(ROOT / "tools/tests/kernel_bundle_guest.c"),
                            str(ROOT / "userland/runtime/shared/boot_slots.c"), "-o", str(binary)], check=True)
            result = subprocess.run(["strace", "-o", str(trace), "-e",
                "trace=mount,umount2,unlink,unlinkat,rename,renameat,renameat2,mkdir,mkdirat,mknod,mknodat,"
                "creat,truncate,ftruncate,pwrite64,fsync,fdatasync,open,openat,execve", str(binary)],
                text=True, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 77, result.stdout + result.stderr)
            self.assertIn("refusing host mutations", result.stderr)
            observed = trace.read_text().splitlines()
            forbidden = re.compile(r"^(mount|umount2|unlink|unlinkat|rename|renameat|renameat2|mkdir|mkdirat|"
                r"mknod|mknodat|creat|truncate|ftruncate|pwrite64|fsync|fdatasync)\(")
            self.assertFalse(any(forbidden.search(line) for line in observed), observed)
            self.assertFalse(any("open" in line and re.search(r"O_(WRONLY|RDWR|CREAT|TRUNC)", line)
                                 for line in observed), observed)
            self.assertEqual(sum(line.startswith("execve(") for line in observed), 1, observed)


class KernelBundle(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="reliefos-runtime-kernel-bundle-")
        cls.addClassCleanup(cls.build.cleanup)
        work = Path(cls.build.name)
        (work / "qa_mbedtls.h").write_text("#define MBEDTLS_SHA256_C\n")
        cls.binary = work / "bundle"
        sources = [ROOT / "userland/runtime/src" / (name + ".c") for name in
                   ("kernel_bundle_io", "kernel_bundle_check", "kernel_bundle_slots", "kernel_bundle_publish", "kernel_bundle_sync", "kernel_bundle_retain", "bootloader_publish")]
        sources.append(ROOT / "userland/runtime/shared/boot_slots.c")
        subprocess.run(["cc", "-std=gnu11", "-D_GNU_SOURCE", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-Wl,--wrap=write,--wrap=pwrite,--wrap=fsync,--wrap=renameat",
                        '-DMBEDTLS_CONFIG_FILE="qa_mbedtls.h"', "-I", str(work), "-I", str(ROOT),
                        "-I", str(ROOT / "userland/runtime/include"), "-I", str(ROOT / "third_party/mbedtls/include"),
                        str(ROOT / "tests/host/test_kernel_bundle.c"), *map(str, sources),
                        str(ROOT / "third_party/mbedtls/library/sha256.c"),
                        str(ROOT / "third_party/mbedtls/library/platform_util.c"), "-o", str(cls.binary)], check=True)

    def setUp(self):
        fixture = fixtures.ModulesStage("test_real_metadata_dependency_closure_and_stale_output")
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        self.fixture = fixture
        fixture.stage()
        self.archive = fixture.work / "early.cpio"
        subprocess.run([str(fixtures.KERNEL_TOOLS), "initramfs-stage", str(ROOT), str(fixture.output),
                        str(fixtures.BUSYBOX), str(fixtures.BUSYBOX.with_name("busybox.links")), str(self.archive), "0", str(fixtures.RECOVERY)], check=True)
        self.payload = fixture.work / "payload"
        subprocess.run([str(fixtures.KERNEL_TOOLS), "kernel-payload", str(fixture.install), str(fixture.output),
                        str(self.archive), str(self.payload)], check=True)
        self.prefix = "usr/lib/reliefos/kernel/" + fixtures.RELEASE + "/"

    def check(self, success=True):
        result = subprocess.run([str(self.binary), "check", str(self.payload), fixtures.RELEASE],
                                text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)

    def rehash(self, member):
        path = self.payload / self.prefix / "SHA256SUMS"
        lines = path.read_text().splitlines()
        checksum = hashlib.sha256((self.payload / member).read_bytes()).hexdigest()
        path.write_text("\n".join(checksum + "  " + member if line[66:] == member else line for line in lines) + "\n")

    def test_complete_bundle_corruption_and_module_order(self):
        self.check()
        module = self.payload / f"lib/modules/{fixtures.RELEASE}/kernel/helper.ko"
        original = module.read_bytes()
        module.write_bytes(original + b"corrupt")
        self.check(False)
        module.write_bytes(original)
        self.check()
        member = f"lib/modules/{fixtures.RELEASE}/modules.order"
        (self.payload / member).write_text("kernel/helper.ko\n")
        self.rehash(member)
        self.check(False)

    def test_symlink_and_nonregular_members_refused_without_blocking(self):
        kernel = self.payload / self.prefix / "kernel.sys"
        original = kernel.read_bytes()
        kernel.unlink()
        os.mkfifo(kernel)
        self.check(False)
        kernel.unlink()
        outside = self.fixture.work / "outside-kernel"
        outside.write_bytes(original)
        kernel.symlink_to(outside)
        self.check(False)

    def test_checksum_list_cannot_escape_or_repeat_members(self):
        sums = self.payload / self.prefix / "SHA256SUMS"
        original = sums.read_bytes()
        sums.write_bytes(original + original.splitlines(keepends=True)[0])
        self.check(False)
        sums.write_bytes(original + ("0" * 64 + "  ../outside\n").encode())
        self.check(False)

    def test_runtime_publication_is_idempotent_and_refuses_before_mutation(self):
        esp = self.fixture.work / "runtime-esp"
        esp.mkdir()
        command = [str(self.binary), "publish", str(self.payload), str(esp), fixtures.RELEASE]
        log = self.fixture.work / "sync-order.log"
        result = subprocess.run(command, text=True, capture_output=True, env={**os.environ, "RELIEFOS_TEST_SYNC_LOG": str(log)})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        events = log.read_text().splitlines()
        first_boot_write = next(i for i, line in enumerate(events) if line.startswith("W " + str(esp) + "/"))
        products = json.loads((self.payload / "kernel-payload.json").read_text())["files"]
        for name in products:
            self.assertIn("S " + str(self.payload / name), events[:first_boot_write], name)
        record = (esp / "reliefos/state/kernel-A.env").read_bytes()
        before = {str(path.relative_to(esp)): path.read_bytes() for path in esp.rglob("*") if path.is_file()}
        subprocess.run(command, check=True)
        self.assertEqual(before, {str(path.relative_to(esp)): path.read_bytes() for path in esp.rglob("*") if path.is_file()})
        self.assertEqual(len(record), 1024)
        self.assertIn(fixtures.RELEASE, subprocess.check_output([str(self.binary), "select", str(esp)], text=True))
        (self.payload / f"lib/modules/{fixtures.RELEASE}/kernel/helper.ko").write_bytes(b"damaged")
        target = self.fixture.work / "refuse-esp"
        target.mkdir()
        result = subprocess.run([str(self.binary), "publish", str(self.payload), str(target), fixtures.RELEASE], capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(list(target.iterdir()), [])

    def test_same_kernel_new_initramfs_keeps_both_pairs_and_metadata_fallback(self):
        esp = self.fixture.work / "same-release-esp"
        esp.mkdir()
        subprocess.run([str(self.binary), "publish", str(self.payload), str(esp), fixtures.RELEASE], check=True)
        old_slot = (esp / "reliefos/state/kernel-A.env").read_bytes()
        old = {str(p.relative_to(esp)): p.read_bytes() for p in (esp / "reliefos/versions").rglob("*") if p.is_file()}
        # Linux newc permits trailing zero padding. This is a different complete
        # early userspace payload with the exact same kernel/module ABI.
        self.archive.write_bytes(self.archive.read_bytes() + bytes(512))
        metadata = self.archive.with_name(self.archive.name + ".json")
        manifest = json.loads(metadata.read_bytes())
        digest = hashlib.sha256(self.archive.read_bytes()).hexdigest()
        manifest["initramfs_sha256"] = digest
        metadata.write_text(json.dumps(manifest))
        new = self.fixture.work / "new-early-userspace"
        subprocess.run([str(fixtures.KERNEL_TOOLS), "kernel-payload", str(self.fixture.install), str(self.fixture.output),
                        str(self.archive), str(new)], check=True)
        result = subprocess.run([str(self.binary), "publish", str(new), str(esp), fixtures.RELEASE], text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(subprocess.check_output([str(self.binary), "select", str(esp)], text=True).strip(), "B 2 " + fixtures.RELEASE)
        self.assertEqual((esp / "reliefos/state/kernel-A.env").read_bytes(), old_slot)
        for path, contents in old.items():
            self.assertEqual((esp / path).read_bytes(), contents, path)
        version = esp / "reliefos/versions" / fixtures.RELEASE / digest
        (version / "boot.env").write_bytes(b"corrupt")
        self.assertEqual(subprocess.check_output([str(self.binary), "select", str(esp)], text=True).strip(), "A 1 " + fixtures.RELEASE)

    def test_interrupted_first_slot_allocation_and_store_can_be_retried(self):
        complete = self.fixture.work / "initial-reference"
        complete.mkdir()
        subprocess.run([str(self.binary), "publish", str(self.payload), str(complete), fixtures.RELEASE], check=True, capture_output=True)
        reference = complete / "reliefos/state"
        for name in ("kernel-A.env", "kernel-A.sha256", "kernel-B.env", "kernel-B.sha256"):
            expected = (reference / name).read_bytes()
            for size in (0, 1, len(expected) // 2, len(expected)):
                with self.subTest(file=name, size=size):
                    target = self.fixture.work / ("initial-" + name + str(size))
                    state = target / "reliefos/state"
                    state.mkdir(parents=True)
                    (state / name).write_bytes(bytes(size))
                    command = [str(self.binary), "publish", str(self.payload), str(target), fixtures.RELEASE]
                    result = subprocess.run(command, capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual(subprocess.check_output([str(self.binary), "select", str(target)], text=True).strip(), "A 1 " + fixtures.RELEASE)
        target = self.fixture.work / "initial-partial-canonical"
        state = target / "reliefos/state"
        state.mkdir(parents=True)
        record = (reference / "kernel-A.env").read_bytes()
        (state / "kernel-A.env").write_bytes(record[:300] + bytes(724))
        subprocess.run([str(self.binary), "publish", str(self.payload), str(target), fixtures.RELEASE], check=True)
        # Unknown nonzero state is not evidence of an interrupted first install.
        target = self.fixture.work / "initial-foreign-state"
        state = target / "reliefos/state"
        state.mkdir(parents=True)
        (state / "kernel-A.env").write_bytes(b"unrelated state")
        result = subprocess.run([str(self.binary), "publish", str(self.payload), str(target), fixtures.RELEASE], capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((state / "kernel-A.env").read_bytes(), b"unrelated state")

    def test_retained_root_package_restores_removed_apk_members_and_keeps_user_files(self):
        command = [str(self.binary), "retain", str(self.payload), fixtures.RELEASE]
        user_module = self.payload / f"lib/modules/{fixtures.RELEASE}/extra/local.ko"
        user_module.parent.mkdir()
        user_module.write_bytes(b"unowned external module")
        products = json.loads((self.payload / "kernel-payload.json").read_bytes())["files"]
        pointer_name = "usr/lib/reliefos/kernel/current-release"
        expected = {name: (self.payload / name).read_bytes() for name in products if name != pointer_name}
        subprocess.run(command, check=True)
        subprocess.run(command, check=True)
        (self.payload / pointer_name).write_text("next-package-release\n")
        for name in expected:
            (self.payload / name).unlink()
        # APK removes only package-owned members; recovery must not erase the
        # independently installed module or change unrelated user files.
        (self.payload / "user-data").write_text("keep user data\n")
        subprocess.run([str(self.binary), "restore", str(self.payload), fixtures.RELEASE], check=True)
        self.check()
        for name, contents in expected.items():
            self.assertEqual((self.payload / name).read_bytes(), contents, name)
        self.assertEqual(user_module.read_bytes(), b"unowned external module")
        self.assertEqual((self.payload / "user-data").read_text(), "keep user data\n")
        self.assertEqual((self.payload / pointer_name).read_text(), "next-package-release\n")
        subprocess.run([str(self.binary), "restore", str(self.payload), fixtures.RELEASE], check=True)

    def test_partial_or_corrupt_retention_cannot_replace_root_products(self):
        command = [str(self.binary), "retain", str(self.payload), fixtures.RELEASE]
        result = subprocess.run(command, env={**os.environ, "RELIEFOS_TEST_FAIL": "1"}, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.check()
        subprocess.run(command, check=True)
        cache = self.payload / "var/lib/reliefos/kernel-retained" / fixtures.RELEASE
        copy = next(cache.glob("*/root/lib/modules/*/kernel/helper.ko"))
        copy.write_bytes(b"corrupt retained module")
        member = self.payload / f"lib/modules/{fixtures.RELEASE}/kernel/helper.ko"
        member.unlink()
        result = subprocess.run([str(self.binary), "restore", str(self.payload), fixtures.RELEASE], capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(member.exists())

    def test_retention_failures_leave_installed_package_valid_and_can_retry(self):
        completed = self.fixture.work / "retention-reference"
        shutil.copytree(self.payload, completed)
        result = subprocess.run([str(self.binary), "retain", str(completed), fixtures.RELEASE],
                                text=True, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        count = int(re.search(r"OPERATIONS=(\d+)", result.stdout)[1])
        self.assertGreater(count, 10)
        products = json.loads((self.payload / "kernel-payload.json").read_bytes())["files"]
        expected = {name: (self.payload / name).read_bytes() for name in products}
        for mode in ("RELIEFOS_TEST_FAIL", "RELIEFOS_TEST_CUT"):
            for operation in range(1, count + 1):
                with self.subTest(mode=mode, operation=operation):
                    target = self.fixture.work / "retention-fault"
                    shutil.copytree(self.payload, target)
                    command = [str(self.binary), "retain", str(target), fixtures.RELEASE]
                    result = subprocess.run(command, text=True, capture_output=True,
                                            env={**os.environ, mode: str(operation)}, timeout=20)
                    self.assertNotEqual(result.returncode, 0, result.stdout)
                    for name, contents in expected.items():
                        self.assertEqual((target / name).read_bytes(), contents, name)
                    result = subprocess.run(command, text=True, capture_output=True, timeout=20)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    for name in expected:
                        if name != "usr/lib/reliefos/kernel/current-release":
                            (target / name).unlink()
                    result = subprocess.run([str(self.binary), "restore", str(target), fixtures.RELEASE],
                                            text=True, capture_output=True, timeout=20)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    for name, contents in expected.items():
                        self.assertEqual((target / name).read_bytes(), contents, name)
                    shutil.rmtree(target)
        print("PASS retention failure/cut/retry/recovery boundaries:", count, "each")

    def test_recovery_failures_can_retry_without_losing_retained_package(self):
        source = self.fixture.work / "recovery-reference"
        shutil.copytree(self.payload, source)
        subprocess.run([str(self.binary), "retain", str(source), fixtures.RELEASE], check=True, capture_output=True)
        products = json.loads((self.payload / "kernel-payload.json").read_bytes())["files"]
        expected = {name: (self.payload / name).read_bytes() for name in products}
        for name in expected:
            if name != "usr/lib/reliefos/kernel/current-release":
                (source / name).unlink()
        accepted = self.fixture.work / "recovery-accepted"
        shutil.copytree(source, accepted)
        result = subprocess.run([str(self.binary), "restore", str(accepted), fixtures.RELEASE],
                                text=True, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        count = int(re.search(r"OPERATIONS=(\d+)", result.stdout)[1])
        self.assertGreater(count, 10)
        for mode in ("RELIEFOS_TEST_FAIL", "RELIEFOS_TEST_CUT"):
            for operation in range(1, count + 1):
                with self.subTest(mode=mode, operation=operation):
                    target = self.fixture.work / "recovery-fault"
                    shutil.copytree(source, target)
                    (target / "unowned-user-file").write_bytes(b"preserve this user file")
                    command = [str(self.binary), "restore", str(target), fixtures.RELEASE]
                    result = subprocess.run(command, text=True, capture_output=True,
                                            env={**os.environ, mode: str(operation)}, timeout=20)
                    self.assertNotEqual(result.returncode, 0, result.stdout)
                    result = subprocess.run(command, text=True, capture_output=True, timeout=20)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    for name, contents in expected.items():
                        self.assertEqual((target / name).read_bytes(), contents, name)
                    self.assertEqual((target / "unowned-user-file").read_bytes(), b"preserve this user file")
                    shutil.rmtree(target)
        print("PASS recovery failure/cut/retry boundaries:", count, "each")

    def test_every_publication_write_sync_and_rename_boundary_preserves_old_slot(self):
        old = self.fixture.work / "baseline-esp"
        old.mkdir()
        subprocess.run([str(self.binary), "publish", str(self.payload), str(old), fixtures.RELEASE], check=True)
        old_files = {str(path.relative_to(old)): path.read_bytes() for path in old.rglob("*")
                     if path.is_file() and ("versions" in path.parts or path.name in ("kernel-A.env", "kernel-A.sha256"))}
        release = "5.1.2-ko1-fedcba987654"
        abi = "fedcba987654" + "0" * 52
        with mock.patch.object(fixtures, "RELEASE", release), mock.patch.object(fixtures, "ABI", abi):
            other = fixtures.ModulesStage("test_real_metadata_dependency_closure_and_stale_output")
            other.setUp()
            self.addCleanup(other.doCleanups)
            other.stage()
            archive = other.work / "early.cpio"
            subprocess.run([str(fixtures.KERNEL_TOOLS), "initramfs-stage", str(ROOT), str(other.output),
                            str(fixtures.BUSYBOX), str(fixtures.BUSYBOX.with_name("busybox.links")), str(archive), "0", str(fixtures.RECOVERY)], check=True)
            new = other.work / "payload"
            subprocess.run([str(fixtures.KERNEL_TOOLS), "kernel-payload", str(other.install), str(other.output), str(archive), str(new)], check=True)
        accepted = self.fixture.work / "accepted-update"
        shutil.copytree(old, accepted)
        command = [str(self.binary), "publish", str(new), str(accepted), release]
        result = subprocess.run(command, text=True, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        count = int(re.search(r"OPERATIONS=(\d+)", result.stdout)[1])
        self.assertGreater(count, 10)
        self.assertEqual(subprocess.check_output([str(self.binary), "select", str(accepted)], text=True).strip(), "B 2 " + release)
        for key in ("RELIEFOS_TEST_FAIL", "RELIEFOS_TEST_CUT", "RELIEFOS_TEST_DURABLE_CUT"):
            for operation in range(1, count + 1):
                with self.subTest(mode=key, operation=operation):
                    target = self.fixture.work / (key + str(operation))
                    shutil.copytree(old, target)
                    environment = {**os.environ, key: str(operation)}
                    durable = self.fixture.work / "durable"
                    if key == "RELIEFOS_TEST_DURABLE_CUT":
                        root_key = snapshot_initial_tree(target, durable)
                        environment.update({"RELIEFOS_TEST_CUT": str(operation), "RELIEFOS_TEST_DURABLE": str(durable)})
                    result = subprocess.run([str(self.binary), "publish", str(new), str(target), release],
                                            env=environment, text=True, capture_output=True, timeout=20)
                    self.assertNotEqual(result.returncode, 0, result.stdout)
                    selected = subprocess.check_output([str(self.binary), "select", str(target)], text=True).strip()
                    self.assertIn(selected, ("A 1 " + fixtures.RELEASE, "B 2 " + release))
                    for name, contents in old_files.items():
                        self.assertEqual((target / name).read_bytes(), contents, name)
                    if key == "RELIEFOS_TEST_DURABLE_CUT":
                        recovered = self.fixture.work / "recovered"
                        restore_durable_tree(durable, root_key, recovered)
                        selected = subprocess.check_output([str(self.binary), "select", str(recovered)], text=True).strip()
                        self.assertIn(selected, ("A 1 " + fixtures.RELEASE, "B 2 " + release))
                        for name, contents in old_files.items():
                            self.assertEqual((recovered / name).read_bytes(), contents, name)
                        shutil.rmtree(recovered)
                        shutil.rmtree(durable)
                    shutil.rmtree(target)
        short = self.fixture.work / "short-writes"
        shutil.copytree(old, short)
        result = subprocess.run([str(self.binary), "publish", str(new), str(short), release],
                                env={**os.environ, "RELIEFOS_TEST_SHORT": "1"}, text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(subprocess.check_output([str(self.binary), "select", str(short)], text=True).strip(), "B 2 " + release)
        print("PASS runtime publication failure/cut/durable-write-loss boundaries:", count, "each; short writes preserved")


if __name__ == "__main__":
    unittest.main()
