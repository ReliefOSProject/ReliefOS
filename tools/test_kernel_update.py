#!/usr/bin/env python3
"""Exercise native system updates with real signed complete kernel packages.

The APK transport is a loopback HTTP server; the bounded release manifest uses
an owned file-copy transport. Neither is an HTTPS/CA test. Actual static target
executables validate, commit and publish A/B payloads in explicit private roots.
Real GRUB launch/confirmation/fallback is covered by test_boot_health_qemu.py.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import threading
import unittest
from unittest import mock

import test_kernel_modules_stage as fixtures
from test_system_update import RepositoryServer
from runtime_test_dependencies import crypto_flags

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = Path(os.environ.get("RELIEFOS_TEST_OUTPUT", ROOT / "out/x86_64/release")).resolve()
APK = Path(os.environ.get("RELIEFOS_TEST_APK", OUTPUT / "upstream/apk/apk.static")).resolve()
BOOTINSTALL = Path(os.environ.get("RELIEFOS_TEST_BOOTINSTALL", OUTPUT / "userland/bootinstall.elf")).resolve()
CLIENT = Path(os.environ.get("RELIEFOS_TEST_SYSTEM_UPDATE", OUTPUT / "userland/systemupdate.elf")).resolve()
fixtures.BUSYBOX = Path(os.environ.get("RELIEFOS_TEST_BUSYBOX", OUTPUT / "userland/initramfs/busybox.elf")).resolve()
fixtures.KERNEL_TOOLS = Path(os.environ.get("RELIEFOS_KERNEL_TOOLS", OUTPUT / "host/bin/reliefos-kernel-tools")).resolve()
fixtures.RECOVERY = Path(os.environ.get("RELIEFOS_TEST_RECOVERY", OUTPUT / "userland/initramfs/kernel-bundle.elf")).resolve()
PLAN_TOKEN = re.compile(r"(?m)^plan_id=([0-9a-f]{64})$")


def sha(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for data in iter(lambda: stream.read(65536), b""):
            digest.update(data)
    return digest.hexdigest()


class KernelUpdate(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        missing = [str(path) for path in (CLIENT, APK, BOOTINSTALL, fixtures.RECOVERY,
                                         fixtures.KERNEL_TOOLS, fixtures.BUSYBOX) if not path.is_file()]
        if missing:
            raise unittest.SkipTest("build native update/kernel tools or set RELIEFOS_TEST_*: " + ", ".join(missing))
        cls.temporary = tempfile.TemporaryDirectory(prefix="reliefos-rpr-signed-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.work = Path(cls.temporary.name)
        # Only uname is replaced for this owned-root confirmation regression;
        # the production CLI, package validation and durable records stay real.
        cls.confirm_cli = cls.work / "confirm-kernel-bundle"
        sources = [ROOT / "userland/runtime/src" / (name + ".c") for name in
                   ("kernel_bundle_io", "kernel_bundle_check", "kernel_bundle_slots", "kernel_bundle_publish",
                    "kernel_bundle_sync", "kernel_bundle_retain", "bootloader_publish")]
        sources.append(ROOT / "userland/runtime/shared/boot_slots.c")
        subprocess.run(["cc", "-std=gnu11", "-D_GNU_SOURCE", "-O1", "-Wall", "-Wextra", "-Werror",
                        "-Wl,--wrap=uname", "-I", ROOT / "userland/runtime/include",
                        ROOT / "userland/apps/kernelbundle/main.c",
                        ROOT / "tests/host/kernel_bundle_cli_environment.c", *sources,
                        *crypto_flags(), "-o", cls.confirm_cli], check=True)
        cls.boot_probe = cls.work / "boot-cmdline-probe"
        subprocess.run(["cc", "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror",
                        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                        "-Wl,--wrap=uname,--wrap=open,--wrap=read",
                        ROOT / "tests/host/test_system_update_boot.c",
                        ROOT / "userland/apps/systemupdate/io.c", *crypto_flags(), "-o", cls.boot_probe], check=True)
        cls.keys = cls.work / "keys"
        cls.keys.mkdir()
        cls.key = cls.work / "qa.rsa"
        subprocess.run(["openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048",
                        "-out", cls.key], check=True, capture_output=True)
        cls.key.chmod(0o600)
        subprocess.run(["openssl", "pkey", "-in", cls.key, "-pubout", "-out", cls.keys / "qa.rsa.pub"],
                       check=True, capture_output=True)
        cls.payloads, cls.repositories, cls.releases = {}, {}, {}
        for name, version, prefix in (("old", "9.0.0-r0", "a" * 12), ("new", "9.0.1-r0", "b" * 12)):
            release, abi = "5.1.2-ko1-" + prefix, prefix + "0" * 52
            cls.releases[name] = release
            with mock.patch.object(fixtures, "RELEASE", release), mock.patch.object(fixtures, "ABI", abi):
                fixture = fixtures.ModulesStage("test_real_metadata_dependency_closure_and_stale_output")
                fixture.setUp()
                try:
                    fixture.stage()
                    archive = fixture.work / "early.cpio"
                    subprocess.run([fixtures.KERNEL_TOOLS, "initramfs-stage", ROOT, fixture.output,
                                    fixtures.BUSYBOX, fixtures.BUSYBOX.with_name("busybox.links"), archive,
                                    "0", fixtures.RECOVERY], check=True, capture_output=True)
                    payload = cls.work / (name + "-payload")
                    subprocess.run([fixtures.KERNEL_TOOLS, "kernel-payload", fixture.install, fixture.output,
                                    archive, payload], check=True, capture_output=True)
                finally:
                    fixture.doCleanups()
            cls.payloads[name] = payload
            repository = cls.work / (name + "-repository")
            repository.mkdir()
            cls.repositories[name] = repository
            # A real exact dependency proves that kernel-only staging is insufficient.
            grub = cls.work / (name + "-grub")
            (grub / "usr/share/reliefos-grub").mkdir(parents=True)
            (grub / "usr/share/reliefos-grub/version").write_text(version + "\n")
            for package, contents in (("reliefos-kernel", payload), ("reliefos-grub", grub)):
                arguments = ["mkpkg", "--files", contents, "--output", repository / (package + "-" + version + ".apk"),
                             "--info", "name:" + package, "--info", "version:" + version,
                             "--info", "arch:x86_64", "--sign-key", cls.key]
                if package == "reliefos-kernel":
                    arguments += ["--info", "depends:reliefos-grub=" + version,
                                  "--script", "post-install:" + str(ROOT / "userland/storage/kernel-package-activate"),
                                  "--script", "post-upgrade:" + str(ROOT / "userland/storage/kernel-package-activate")]
                cls.apk(*arguments)
            cls.apk("mkndx", "--keys-dir", cls.keys, "--sign-key", cls.key, "--output",
                    repository / "packages.adb", *sorted(repository.glob("*.apk")))
        cls.base = cls.work / "base"
        for path in ("bin", "sbin", "usr/sbin", "usr/libexec/reliefos", "etc/apk/keys", "etc/reliefos",
                     "lib/apk/db", "lib/apk/commit_hooks.d", "run", "tmp", "dev", "boot/grub", "serve",
                     "var/lib/reliefos", "var/cache/apk"):
            (cls.base / path).mkdir(parents=True, exist_ok=True)
        shutil.copy2(fixtures.BUSYBOX, cls.base / "bin/busybox")
        applets = subprocess.check_output([fixtures.BUSYBOX, "--list"], text=True).splitlines()
        for name in applets:
            if name != "busybox":
                (cls.base / "bin" / name).symlink_to("busybox")
        shutil.copy2(APK, cls.base / "sbin/apk")
        shutil.copy2(fixtures.RECOVERY, cls.base / "usr/libexec/reliefos/kernel-bundle")
        shutil.copy2(CLIENT, cls.base / "usr/sbin/reliefos-update")
        (cls.base / "lib/apk/commit_hooks.d/reliefos-update").symlink_to("/usr/sbin/reliefos-update")
        shutil.copy2(ROOT / "system/rootfs/lib/apk/commit_hooks.d/reliefos-kernel",
                     cls.base / "lib/apk/commit_hooks.d/reliefos-kernel")
        shutil.copy2(cls.keys / "qa.rsa.pub", cls.base / "etc/apk/keys/qa.rsa.pub")
        (cls.base / "etc/apk/world").write_text("")
        (cls.base / "etc/apk/arch").write_text("x86_64\n")
        configuration = cls.work / "old.repositories"
        configuration.write_text("ndx " + str(cls.repositories["old"] / "packages.adb") + "\n")
        cls.apk("--root", cls.base, "--repositories-file", configuration, "--no-network", "--no-scripts",
                "--initdb", "add", "reliefos-kernel")
        subprocess.run([fixtures.RECOVERY, "--retain-installed", cls.base], check=True, capture_output=True)
        (cls.base / "boot/grub/kernel-slots.cfg").write_bytes((ROOT / "boot/grub/kernel-slots.cfg").read_bytes())
        (cls.base / "boot/grub/grub.cfg").write_text("# preserved old rollback entry\n")
        (cls.base / "boot/EFI/BOOT").mkdir(parents=True)
        (cls.base / "boot/EFI/BOOT/BOOTX64.EFI").write_bytes(b"preserved firmware\n")
        subprocess.run([BOOTINSTALL, "--kernel-release", cls.releases["old"],
                        "--root-directory", cls.base, "--boot-directory", cls.base / "boot"],
                       check=True, capture_output=True)

    @classmethod
    def apk(cls, *arguments, success=True):
        result = subprocess.run(["unshare", "-Urmp", "--fork", APK, *map(str, arguments)],
                                text=True, capture_output=True, timeout=60)
        if success and result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        return result

    def setUp(self):
        self.temporary_case = tempfile.TemporaryDirectory(prefix="case-", dir=self.work)
        self.addCleanup(self.temporary_case.cleanup)
        self.tree = Path(self.temporary_case.name) / "root"
        shutil.copytree(self.base, self.tree, symlinks=True)
        shutil.copytree(self.repositories["new"], self.tree / "serve/apk")
        (self.tree / "serve/kernel").mkdir()
        self.server = RepositoryServer(self.tree / "serve")
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.addCleanup(self.stop_server)
        self.url = "http://127.0.0.1:" + str(self.server.server_port)
        (self.tree / "etc/apk/repositories").write_text("ndx " + self.url + "/apk/packages.adb\n")
        (self.tree / "etc/reliefos/rpr.conf").write_text("RPR_BASE_URL=" + self.url + "\n")
        fetch = self.tree / "usr/sbin/rprfetch"
        fetch.write_text("#!/bin/sh\nset -eu\n[ \"$1\" = '" + self.url + "/kernel/release.txt' ] || exit 2\n"
                         "printf '%s\\n' kernel/release.txt >> '" + str(self.tree / "run/fetched") + "'\n"
                         "cp '" + str(self.tree / "serve/kernel/release.txt") + "' \"$2\"\n")
        fetch.chmod(0o755)
        self.manifest = self.tree / "serve/kernel/release.txt"
        prefix = self.payloads["new"] / "usr/lib/reliefos/kernel" / self.releases["new"]
        result = subprocess.run([fixtures.KERNEL_TOOLS, "release-info", self.payloads["new"], prefix / "kernel.sys",
            self.tree / "serve/apk/reliefos-kernel-9.0.1-r0.apk", self.tree / "serve/apk/packages.adb", "5.1.2", fixtures.RECOVERY],
            check=True, text=True, capture_output=True)
        self.manifest.write_text(result.stdout)
        self.before = {path: sha(self.tree / path) for path in (
            "etc/apk/world", "lib/apk/db/installed", "boot/grub/grub.cfg", "boot/EFI/BOOT/BOOTX64.EFI",
            "boot/reliefos/state/kernel-A.env", "boot/reliefos/state/kernel-A.sha256",
            "boot/reliefos/state/kernel-B.env", "boot/reliefos/state/kernel-B.sha256",
            "boot/reliefos/state/kernel-A.health",
            "boot/reliefos/state/boot-health-v1")}
        self.boot_before = self.boot_status()
        self.output = ""

    def stop_server(self):
        if self.server is not None:
            self.server.shutdown()
            self.server.server_close()
            self.thread.join(timeout=5)
            self.server = None

    def update(self, *arguments):
        result = subprocess.run(["unshare", "-Urm", "--fork", CLIENT, "--root", self.tree,
                                 *map(str, arguments)], text=True, capture_output=True, timeout=90)
        self.output = result.stdout + result.stderr
        return result

    def prepare(self, command="prepare"):
        result = self.update(command, "--machine")
        self.assertEqual(result.returncode, 0, self.output)
        plans = PLAN_TOKEN.findall(result.stdout)
        self.assertEqual(len(plans), 1, self.output)
        self.unchanged()
        return plans[0]

    def apply(self, plan):
        return self.update("apply", "--plan", plan, "--machine")

    def boot_status(self):
        result = subprocess.run([fixtures.RECOVERY, "--boot-status", self.tree, self.tree / "boot"],
                                check=True, text=True, capture_output=True, timeout=20)
        return dict(line.split("=", 1) for line in result.stdout.splitlines())

    def assert_new_trial(self):
        status = self.boot_status()
        self.assertEqual(status["latest_release"], self.releases["new"])
        self.assertEqual(status["latest_confirmed"], "0")
        self.assertEqual(status["latest_remaining"], "1")
        for name in ("slot", "generation", "release", "initramfs"):
            self.assertEqual(status["confirmed_" + name], self.boot_before["confirmed_" + name])
        self.assertGreater(int(status["latest_generation"]), int(self.boot_before["latest_generation"]))

    def unchanged(self, database=True, slots=True):
        for path, digest in self.before.items():
            if not database and path in ("etc/apk/world", "lib/apk/db/installed"):
                continue
            if not slots and path.startswith("boot/reliefos/state/"):
                continue
            self.assertEqual(sha(self.tree / path), digest, path + "\n" + self.output)
        self.assertFalse((self.tree / "run/reliefos-kernel-update.lock").exists())
        self.assertFalse((self.tree / "run/leonos-kernel-update.lock").exists())
        self.assertEqual(list((self.tree / "tmp").iterdir()), [])

    def field(self, key, value):
        fields = dict(line.split("=", 1) for line in self.manifest.read_text().splitlines())
        fields[key] = value
        self.manifest.write_text("".join(name + "=" + data + "\n" for name, data in fields.items()))

    def reject(self, diagnostic=None):
        result = self.update("prepare", "--machine")
        self.assertNotEqual(result.returncode, 0, self.output)
        self.assertFalse(PLAN_TOKEN.search(result.stdout), self.output)
        if diagnostic:
            self.assertIn(diagnostic, self.output)
        self.unchanged()
        # A rejected preparation owns persistent diagnostics until discarded.
        result = self.update("discard", "--machine")
        self.assertEqual(result.returncode, 0, self.output)

    def reindex(self, signed=True):
        repo = self.tree / "serve/apk"
        arguments = ["mkndx", "--keys-dir", self.keys, "--output", repo / "packages.adb"]
        if signed:
            arguments += ["--sign-key", self.key]
        else:
            arguments.insert(0, "--allow-untrusted")  # A deliberately invalid negative fixture.
        self.apk(*arguments, *sorted(repo.glob("*.apk")))
        self.field("repository_sha256", sha(repo / "packages.adb"))

    def repack_kernel(self, payload, script=None):
        archive = self.tree / "serve/apk/reliefos-kernel-9.0.1-r0.apk"
        archive.unlink()
        arguments = ["mkpkg", "--files", payload, "--output", archive, "--info", "name:reliefos-kernel",
                     "--info", "version:9.0.1-r0", "--info", "arch:x86_64", "--info", "depends:reliefos-grub=9.0.1-r0",
                     "--sign-key", self.key]
        if script:
            arguments += ["--script", "post-upgrade:" + str(script)]
        self.apk(*arguments)
        self.field("package_sha256", sha(archive))
        self.reindex()

    def describe_payload(self, payload, release):
        prefix = payload / "usr/lib/reliefos/kernel" / release
        result = subprocess.run([fixtures.KERNEL_TOOLS, "release-info", payload, prefix / "kernel.sys",
            self.tree / "serve/apk/reliefos-kernel-9.0.1-r0.apk", self.tree / "serve/apk/packages.adb",
            "5.1.2", fixtures.RECOVERY], check=True, text=True, capture_output=True)
        self.manifest.write_text(result.stdout)

    def publish_payload(self, payload):
        """Model a previously published trial without changing the installed DB."""
        subprocess.run([fixtures.RECOVERY, "--publish-installed", payload, self.tree / "boot"],
                       check=True, capture_output=True)
        self.boot_before = self.boot_status()
        self.before = {path: sha(self.tree / path) for path in self.before}

    def consume_trial(self):
        status = self.boot_status()
        health = self.tree / "boot/reliefos/state" / ("kernel-" + status["latest_slot"] + ".health")
        health.write_bytes(health.read_bytes().replace(b"reliefos_boot_remaining=1", b"reliefos_boot_remaining=0"))
        self.boot_before = self.boot_status()
        self.before = {path: sha(self.tree / path) for path in self.before}

    def interrupted_after_publication(self, plan):
        directory = self.tree / "var/lib/reliefos/update/plans" / plan
        state = directory / "state"
        state.write_text(state.read_text().replace("phase=complete\n", "phase=verify-failed\n"))

    def confirm_trial(self):
        status = self.boot_status()
        self.consume_trial()
        proc = self.tree / "proc"
        proc.mkdir(exist_ok=True)
        (proc / "cmdline").write_text("reliefos.boot_slot=" + status["latest_slot"] +
            " reliefos.boot_generation=" + status["latest_generation"] +
            " reliefos.boot_initramfs=" + status["latest_initramfs"] + "\n")
        result = subprocess.run([self.confirm_cli, "--confirm-running", self.tree, self.tree / "boot"],
            env={**os.environ, "RELIEFOS_TEST_UNAME_RELEASE": status["latest_release"]},
            text=True, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.boot_status()["latest_confirmed"], "1")

    def install_publication_probe(self, behavior):
        """Inject only the helper boundary; signatures/solver/validator stay real."""
        real = self.tree / "usr/libexec/reliefos/kernel-bundle.real"
        helper = self.tree / "usr/libexec/reliefos/kernel-bundle"
        shutil.copy2(helper, real)
        if behavior == "noop":
            body = '[ "$1" != --publish-installed ] || exit 0\nexec "' + str(real) + '" "$@"\n'
        elif behavior == "wrong-status":
            published = self.tree / "run/probe-published"
            body = ('if [ "$1" = --publish-installed ]; then\n    "' + str(real) + '" "$@"\n    touch "' +
                    str(published) + '"\nelif [ "$1" = --boot-status ] && [ -f "' + str(published) + '" ]; then\n    "' + str(real) + '" "$@" | '
                    "sed 's/^latest_release=.*/latest_release=incorrect/'\nelse\n    exec \"" +
                    str(real) + '\" "$@"\nfi\n')
        else:
            raise AssertionError(behavior)
        helper.write_text("#!/bin/sh\nset -eu\n" + body)
        helper.chmod(0o755)

    def test_signed_complete_package_and_dependencies_without_world_pin(self):
        plan = self.prepare()
        result = self.apply(plan)
        self.assertEqual(result.returncode, 0, self.output)
        self.assertIn("reboot_required=1", self.output)
        self.assertEqual((self.tree / "etc/apk/world").read_text(), "reliefos-kernel\n")
        self.assertEqual((self.tree / "usr/share/reliefos-grub/version").read_text(), "9.0.1-r0\n")
        result = subprocess.run([fixtures.RECOVERY, "--inspect", self.tree, self.releases["new"]], capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.unchanged(database=False, slots=False)
        self.assert_new_trial()
        fetched = "\n".join(self.server.requests)
        self.assertIn("apk/reliefos-kernel-9.0.1-r0.apk", fetched)
        self.assertIn("apk/reliefos-grub-9.0.1-r0.apk", fetched)
        self.assertNotIn("kernel/kernel.sys", fetched)
        self.assertEqual((self.tree / "run/fetched").read_text(), "kernel/release.txt\n")
        self.assertFalse((self.tree / "run/reliefos-kernel-activation.defer").exists())

    def test_real_root_fallback_guard_reads_proc_stream_without_stat_size(self):
        result = subprocess.run([self.boot_probe], text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_prepared_complete_kernel_applies_without_network(self):
        plan = self.prepare()
        self.stop_server()
        result = self.apply(plan)
        self.assertEqual(result.returncode, 0, self.output)
        self.assert_new_trial()

    def test_check_prepares_complete_plan_without_installed_root_changes(self):
        plan = self.prepare("check")
        directory = self.tree / "var/lib/reliefos/update/plans" / plan
        self.assertTrue(list((directory / "cache").glob("*.apk")))
        self.assertTrue((directory / "kernel-identity").is_file())
        self.assertIn("phase=ready\n", (directory / "state").read_text())
        self.unchanged()

    def test_formats_one_two_and_three_are_refused_without_half_update(self):
        original = self.manifest.read_bytes()
        for number in (1, 2, 3):
            with self.subTest(format=number):
                self.manifest.write_bytes(original)
                self.field("format_version", str(number))
                self.reject()

    def test_duplicate_unknown_and_binary_fields_are_refused(self):
        original = self.manifest.read_bytes()
        for suffix in (b"format_version=4\n", b"unknown=value\n", b"\0"):
            with self.subTest(suffix=suffix):
                self.manifest.write_bytes(original + suffix)
                self.reject()

    def test_image_release_disagreement_is_refused(self):
        self.field("image_version", "5.1.3")
        self.reject()

    def test_path_and_package_pairing_are_refused(self):
        self.field("package_file", "../reliefos-kernel-9.0.1-r0.apk")
        self.reject()

    def test_frozen_index_race_is_refused(self):
        # A valid new signed publication must not be combined with the old
        # transport manifest, even when both independently verify.
        old = self.repositories["old"] / "reliefos-kernel-9.0.0-r0.apk"
        shutil.copy2(old, self.tree / "serve/apk" / old.name)
        repository_hash = dict(line.split("=", 1) for line in self.manifest.read_text().splitlines())["repository_sha256"]
        self.reindex()
        self.field("repository_sha256", repository_hash)
        self.reject()

    def test_unsigned_index_is_refused_even_with_matching_transport_hash(self):
        self.reindex(signed=False)
        self.reject()

    def test_archive_checksum_failure_is_refused_before_apk_commit(self):
        archive = self.tree / "serve/apk/reliefos-kernel-9.0.1-r0.apk"
        contents = bytearray(archive.read_bytes())
        contents[len(contents) // 2] ^= 1
        archive.write_bytes(contents)
        self.reject()

    def test_corrupt_module_in_new_signed_package_is_refused(self):
        payload = Path(self.temporary_case.name) / "corrupt-payload"
        shutil.copytree(self.payloads["new"], payload)
        module = payload / "lib/modules" / self.releases["new"] / "kernel/rootfs.ko"
        module.write_bytes(module.read_bytes() + b"signed but inconsistent module")
        self.repack_kernel(payload)
        self.reject("kernel-bundle:")

    def test_advertised_abi_and_signed_bundle_must_agree(self):
        self.field("module_abi", "c" * 64)
        self.reject()

    def test_advertised_kernel_initramfs_and_manifest_hashes_must_agree(self):
        original = self.manifest.read_bytes()
        for field in ("kernel_sha256", "initramfs_sha256", "bundle_manifest_sha256"):
            with self.subTest(field=field):
                self.manifest.write_bytes(original)
                self.field(field, "c" * 64)
                self.reject()

    def test_multiple_kernel_candidates_follow_signed_solver_selection(self):
        old = self.repositories["old"] / "reliefos-kernel-9.0.0-r0.apk"
        shutil.copy2(old, self.tree / "serve/apk" / old.name)
        self.reindex()
        plan = self.prepare()
        result = self.apply(plan)
        self.assertEqual(result.returncode, 0, self.output)
        self.assert_new_trial()

    def test_advertised_package_revision_must_match_solver_selection(self):
        self.field("package_version", "9.0.2-r0")
        self.field("package_file", "reliefos-kernel-9.0.2-r0.apk")
        self.reject()

    def test_missing_signed_dependency_is_refused(self):
        (self.tree / "serve/apk/reliefos-grub-9.0.1-r0.apk").unlink()
        self.reindex()
        self.reject()

    def test_legacy_loader_requires_installer_migration_before_commit(self):
        (self.tree / "boot/grub/kernel-slots.cfg").unlink()
        self.reject("boot resources require migration")

    def test_failed_package_script_preserves_boot_and_retained_old_bundle(self):
        failure = Path(self.temporary_case.name) / "activation-failure"
        failure.write_text("#!/bin/sh\necho 'injected activation failure' >&2\nexit 1\n")
        failure.chmod(0o755)
        self.repack_kernel(self.payloads["new"], failure)
        plan = self.prepare()
        result = self.apply(plan)
        self.assertNotEqual(result.returncode, 0, self.output)
        self.assertIn("injected activation failure", self.output)
        self.unchanged(database=False)
        result = subprocess.run([fixtures.RECOVERY, "--restore-all", self.tree], text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for name in ("old", "new"):
            result = subprocess.run([fixtures.RECOVERY, "--inspect", self.tree, self.releases[name]], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.boot_status(), self.boot_before)
        result = self.update("repair", "--machine")
        self.assertNotEqual(result.returncode, 0, self.output)
        self.assertIn("injected activation failure", self.output)
        self.assertEqual(self.boot_status(), self.boot_before)

    def test_failed_package_script_can_repair_offline_before_boot_publication(self):
        failure = Path(self.temporary_case.name) / "repairable-activation"
        failure.write_text("#!/bin/sh\n[ -f /run/fixture-allow-commit ] || exit 19\n")
        failure.chmod(0o755)
        self.repack_kernel(self.payloads["new"], failure)
        plan = self.prepare()
        result = self.apply(plan)
        self.assertNotEqual(result.returncode, 0, self.output)
        self.assertEqual(self.boot_status(), self.boot_before)
        (self.tree / "run/fixture-allow-commit").write_text("explicit repair may finish\n")
        self.stop_server()
        result = self.update("repair", "--machine")
        self.assertEqual(result.returncode, 0, self.output)
        self.assert_new_trial()
        self.unchanged(database=False, slots=False)

    def test_successful_noop_publication_is_reported_as_verification_failure(self):
        self.install_publication_probe("noop")
        plan = self.prepare()
        result = self.apply(plan)
        self.assertNotEqual(result.returncode, 0, self.output)
        self.assertEqual(self.boot_status(), self.boot_before)
        result = self.update("status", "--machine")
        self.assertEqual(result.returncode, 0, self.output)
        self.assertIn("phase=verify-failed", self.output)

    def test_mismatched_boot_status_is_reported_as_verification_failure(self):
        self.install_publication_probe("wrong-status")
        plan = self.prepare()
        result = self.apply(plan)
        self.assertNotEqual(result.returncode, 0, self.output)
        # The real helper independently confirms publication happened. The
        # updater must still refuse a mismatched result from its own helper.
        self.assert_new_trial()
        result = self.update("status", "--machine")
        self.assertEqual(result.returncode, 0, self.output)
        self.assertIn("phase=verify-failed", self.output)

    def test_current_package_noop_and_completed_apply_are_idempotent(self):
        shutil.rmtree(self.tree / "serve/apk")
        shutil.copytree(self.repositories["old"], self.tree / "serve/apk")
        prefix = self.payloads["old"] / "usr/lib/reliefos/kernel" / self.releases["old"]
        result = subprocess.run([fixtures.KERNEL_TOOLS, "release-info", self.payloads["old"], prefix / "kernel.sys",
            self.tree / "serve/apk/reliefos-kernel-9.0.0-r0.apk", self.tree / "serve/apk/packages.adb", "5.1.2", fixtures.RECOVERY],
            check=True, text=True, capture_output=True)
        self.manifest.write_text(result.stdout)
        plan = self.prepare()
        result = self.apply(plan)
        self.assertEqual(result.returncode, 0, self.output)
        self.assertIn("reboot_required=0", self.output)
        self.unchanged()
        # APK's native cache command also caches unchanged installed packages
        # for offline repairs. A no-op guarantees installed/boot identities,
        # rather than promising an empty download set.
        self.assertFalse((self.tree / "run/fetched").exists())
        self.assertEqual(self.apply(plan).returncode, 0, self.output)
        self.assertEqual((self.tree / "etc/apk/world").read_text(), "reliefos-kernel\n")
        self.unchanged()

    def test_new_apk_revision_with_same_confirmed_payload_needs_no_trial(self):
        self.repack_kernel(self.payloads["old"])
        self.describe_payload(self.payloads["old"], self.releases["old"])
        plan = self.prepare()
        result = self.apply(plan)
        self.assertEqual(result.returncode, 0, self.output)
        self.assertIn("reboot_required=0", self.output)
        self.assertEqual(self.boot_status(), self.boot_before)
        self.assertEqual((self.tree / "usr/share/reliefos-grub/version").read_text(), "9.0.1-r0\n")
        self.unchanged(database=False)

    def test_interrupted_published_update_repairs_after_boot_confirmation(self):
        plan = self.prepare()
        self.assertEqual(self.apply(plan).returncode, 0, self.output)
        self.interrupted_after_publication(plan)
        self.confirm_trial()
        confirmed = self.boot_status()
        self.stop_server()
        result = self.update("repair", "--machine")
        self.assertEqual(result.returncode, 0, self.output)
        self.assertIn("reboot_required=0", self.output)
        self.assertEqual(self.boot_status(), confirmed)

    def test_interrupted_consumed_trial_repair_keeps_fallback_and_attempt_spent(self):
        plan = self.prepare()
        self.assertEqual(self.apply(plan).returncode, 0, self.output)
        self.interrupted_after_publication(plan)
        self.consume_trial()
        failed = self.boot_status()
        self.stop_server()
        result = self.update("repair", "--machine")
        self.assertEqual(result.returncode, 0, self.output)
        self.assertIn("next automatic boot uses the confirmed recovery kernel", self.output)
        self.assertEqual(self.boot_status(), failed)

    def test_failed_trial_cannot_be_rearmed_by_another_plan(self):
        self.publish_payload(self.payloads["new"])
        self.consume_trial()
        self.reject("this kernel trial already failed")
        self.assertEqual(self.boot_status(), self.boot_before)

    def test_failed_trial_can_be_replaced_by_a_different_signed_candidate(self):
        self.publish_payload(self.payloads["new"])
        self.consume_trial()
        failed = self.boot_status()
        # A signed higher package revision may restore the known-good payload;
        # its identity differs from the exhausted candidate, without touching
        # the confirmed slot used as the fallback for the new publication.
        self.repack_kernel(self.payloads["old"])
        self.describe_payload(self.payloads["old"], self.releases["old"])
        plan = self.prepare()
        result = self.apply(plan)
        self.assertEqual(result.returncode, 0, self.output)
        status = self.boot_status()
        self.assertEqual(status["latest_release"], self.releases["old"])
        self.assertEqual(status["latest_confirmed"], "0")
        self.assertEqual(status["latest_remaining"], "1")
        self.assertGreater(int(status["latest_generation"]), int(failed["latest_generation"]))
        for name in ("slot", "generation", "release", "initramfs"):
            self.assertEqual(status["confirmed_" + name], failed["confirmed_" + name])

    def test_pending_trial_must_finish_before_another_kernel_plan(self):
        self.publish_payload(self.payloads["new"])
        self.repack_kernel(self.payloads["old"])
        self.describe_payload(self.payloads["old"], self.releases["old"])
        self.reject("previous kernel trial is unconfirmed")

    def test_rpr_producer_rejects_different_signed_kernel_and_described_payload(self):
        directory = Path(self.temporary_case.name)
        repository, apps = directory / "producer-repo", directory / "producer-apps"
        repository.mkdir()
        apps.mkdir()
        shutil.copy2(self.repositories["new"] / "reliefos-kernel-9.0.1-r0.apk", repository)
        shutil.copy2(self.repositories["new"] / "reliefos-grub-9.0.1-r0.apk", apps)
        version = directory / "version"
        version.write_text("release_version=5.1.2\n")
        prefix = self.payloads["old"] / "usr/lib/reliefos/kernel" / self.releases["old"]
        result = subprocess.run(["sh", ROOT / "tools/build/rpr-pages.sh", repository, apps, prefix / "kernel.sys",
            self.payloads["old"], version, APK, self.key, directory / "published"], text=True, capture_output=True,
            env={**os.environ, "RELIEFOS_KERNEL_TOOLS": str(fixtures.KERNEL_TOOLS), "RELIEFOS_KERNEL_RECOVERY": str(fixtures.RECOVERY)})
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((directory / "published").exists())
        # A matching signed archive must still publish through the same code.
        prefix = self.payloads["new"] / "usr/lib/reliefos/kernel" / self.releases["new"]
        working = directory / "relative build"
        working.mkdir()
        for payload in (self.payloads["new"], Path(os.path.relpath(self.payloads["new"], working))):
            with self.subTest(payload=str(payload)):
                result = subprocess.run(["sh", ROOT / "tools/build/rpr-pages.sh", repository, apps,
                    prefix / "kernel.sys", payload, version, APK, self.key, directory / "published"],
                    cwd=working, text=True, capture_output=True,
                    env={**os.environ, "RELIEFOS_KERNEL_TOOLS": str(fixtures.KERNEL_TOOLS),
                         "RELIEFOS_KERNEL_RECOVERY": str(fixtures.RECOVERY)})
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                metadata = json.loads((directory / "published/kernel/release.json").read_bytes())
                self.assertEqual(metadata["schema"], 4)
                self.assertEqual(metadata["kernel_release"], self.releases["new"])


if __name__ == "__main__":
    unittest.main()
