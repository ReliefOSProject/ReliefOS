#!/usr/bin/env python3
"""Exercise the RPR client with upstream APK, signed native bundles and private roots.

rprfetch serves owned local fixtures, so this does not claim an HTTPS/CA test.
Linux-hosted APK scripts deliberately defer ESP activation; actual ReliefNT
commit hooks, A/B publication and reboot are covered by the owned guest matrix.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

import test_kernel_modules_stage as fixtures

ROOT = Path(__file__).resolve().parents[1]
APK = Path(os.environ.get("RELIEFOS_TEST_APK", ROOT / "out/kbuild-os-native/upstream/apk/apk.static"))
BOOTINSTALL = Path(os.environ.get("RELIEFOS_TEST_BOOTINSTALL", ROOT / "out/kbuild-os-native/userland/bootinstall.elf"))
CLIENT = ROOT / "userland/storage/leonos-kernel-update"


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


class KernelUpdate(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="reliefos-rpr-signed-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.work = Path(cls.temporary.name)
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
                     "lib/apk/db", "lib/apk/commit_hooks.d", "run", "tmp", "dev", "boot/grub", "serve"):
            (cls.base / path).mkdir(parents=True, exist_ok=True)
        shutil.copy2(fixtures.BUSYBOX, cls.base / "bin/busybox")
        applets = subprocess.check_output([fixtures.BUSYBOX, "--list"], text=True).splitlines()
        for name in applets:
            if name != "busybox":
                (cls.base / "bin" / name).symlink_to("busybox")
        shutil.copy2(APK, cls.base / "sbin/apk")
        shutil.copy2(fixtures.RECOVERY, cls.base / "usr/libexec/reliefos/kernel-bundle")
        shutil.copy2(cls.keys / "qa.rsa.pub", cls.base / "etc/apk/keys/qa.rsa.pub")
        (cls.base / "etc/apk/world").write_text("")
        (cls.base / "etc/apk/arch").write_text("x86_64\n")
        configuration = cls.work / "old.repositories"
        configuration.write_text("ndx " + str(cls.repositories["old"] / "packages.adb") + "\n")
        cls.apk("--root", cls.base, "--repositories-file", configuration, "--no-network", "--no-scripts",
                "--initdb", "add", "reliefos-kernel")
        subprocess.run([fixtures.RECOVERY, "--retain-installed", cls.base], check=True, capture_output=True)
        (cls.base / "etc/reliefos/rpr.conf").write_text("RPR_BASE_URL=https://rpr.test.invalid\n")
        (cls.base / "boot/grub/kernel-slots.cfg").write_bytes((ROOT / "boot/grub/kernel-slots.cfg").read_bytes())
        (cls.base / "boot/grub/grub.cfg").write_text("# preserved old rollback entry\n")
        (cls.base / "boot/EFI/BOOT").mkdir(parents=True)
        (cls.base / "boot/EFI/BOOT/BOOTX64.EFI").write_bytes(b"preserved firmware\n")
        fetch = cls.base / "usr/sbin/rprfetch"
        fetch.write_text("#!/bin/sh\nset -eu\ncase $1 in https://rpr.test.invalid/*) ;; *) exit 1;; esac\n"
                         "relative=${1#https://rpr.test.invalid/}\n"
                         "printf '%s\\n' \"$relative\" >> /run/fetched\ncp \"/serve/$relative\" \"$2\"\n")
        fetch.chmod(0o755)
        subprocess.run([BOOTINSTALL, "--kernel-release", cls.releases["old"],
                        "--root-directory", cls.base, "--boot-directory", cls.base / "boot"],
                       check=True, capture_output=True)

    @classmethod
    def apk(cls, *arguments, success=True):
        result = subprocess.run(["unshare", "-Ur", APK, *map(str, arguments)], text=True, capture_output=True)
        if success and result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        return result

    def setUp(self):
        self.temporary_case = tempfile.TemporaryDirectory(prefix="case-", dir=self.work)
        self.addCleanup(self.temporary_case.cleanup)
        self.tree = Path(self.temporary_case.name) / "root"
        shutil.copytree(self.base, self.tree, symlinks=True)
        shutil.copy2(CLIENT, self.tree / "usr/sbin/reliefos-kernel-update")
        (self.tree / "usr/sbin/reliefos-kernel-update").chmod(0o755)
        shutil.copytree(self.repositories["new"], self.tree / "serve/apk")
        (self.tree / "serve/kernel").mkdir()
        self.manifest = self.tree / "serve/kernel/release.txt"
        prefix = self.payloads["new"] / "usr/lib/reliefos/kernel" / self.releases["new"]
        result = subprocess.run([fixtures.KERNEL_TOOLS, "release-info", self.payloads["new"], prefix / "kernel.sys",
            self.tree / "serve/apk/reliefos-kernel-9.0.1-r0.apk", self.tree / "serve/apk/packages.adb", "5.1.2", fixtures.RECOVERY],
            check=True, text=True, capture_output=True)
        self.manifest.write_text(result.stdout)
        self.before = {path: sha(self.tree / path) for path in (
            "etc/apk/world", "lib/apk/db/installed", "boot/grub/grub.cfg", "boot/EFI/BOOT/BOOTX64.EFI",
            "boot/reliefos/state/kernel-A.env", "boot/reliefos/state/kernel-A.sha256",
            "boot/reliefos/state/kernel-B.env", "boot/reliefos/state/kernel-B.sha256")}

    def update(self, *arguments):
        # A private pivot root admits upstream APK's own Linux script sandbox.
        # All tools inside it are actual static target executables.
        result = subprocess.run(["unshare", "-Urmp", "--fork", "sh", "-eu", "-c",
            'mount --make-rprivate /; mount --bind "$1" "$1"; touch "$1/dev/null"; '
            'mount --bind /dev/null "$1/dev/null"; cd "$1"; mkdir -p .oldroot; pivot_root . .oldroot; '
            'shift; exec /usr/sbin/reliefos-kernel-update "$@"', "sh", self.tree, *arguments],
            text=True, capture_output=True, timeout=45, env={**os.environ, "TMPDIR": "/tmp"})
        self.output = result.stdout + result.stderr
        return result

    def unchanged(self, database=True):
        for path, digest in self.before.items():
            if not database and path in ("etc/apk/world", "lib/apk/db/installed"):
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
        result = self.update()
        self.assertNotEqual(result.returncode, 0, self.output)
        if diagnostic:
            self.assertIn(diagnostic, self.output)
        self.unchanged()

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

    def test_signed_complete_package_and_dependencies_without_world_pin(self):
        result = self.update()
        self.assertEqual(result.returncode, 0, self.output)
        self.assertIn("Installed kernel", self.output)
        self.assertEqual((self.tree / "etc/apk/world").read_text(), "reliefos-kernel\n")
        self.assertEqual((self.tree / "usr/share/reliefos-grub/version").read_text(), "9.0.1-r0\n")
        result = subprocess.run([fixtures.RECOVERY, "--inspect", self.tree, self.releases["new"]], capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.unchanged(database=False)
        fetched = (self.tree / "run/fetched").read_text()
        self.assertIn("apk/reliefos-kernel-9.0.1-r0.apk", fetched)
        self.assertIn("apk/reliefos-grub-9.0.1-r0.apk", fetched)
        self.assertNotIn("kernel/kernel.sys", fetched)

    def test_check_reports_package_revision_with_no_root_or_archive_changes(self):
        result = self.update("--check")
        self.assertEqual(result.returncode, 0, self.output)
        self.assertIn("update available", self.output)
        self.unchanged()
        self.assertNotIn(".apk", (self.tree / "run/fetched").read_text())

    def test_formats_one_two_and_three_are_refused_without_half_update(self):
        original = self.manifest.read_bytes()
        for number in (1, 2, 3):
            with self.subTest(format=number):
                self.manifest.write_bytes(original)
                self.field("format_version", str(number))
                self.reject("Unsupported kernel release manifest")

    def test_duplicate_unknown_and_binary_fields_are_refused(self):
        original = self.manifest.read_bytes()
        for suffix in (b"format_version=4\n", b"unknown=value\n", b"\0"):
            with self.subTest(suffix=suffix):
                self.manifest.write_bytes(original + suffix)
                self.reject()

    def test_image_release_disagreement_is_refused(self):
        self.field("image_version", "5.1.3")
        self.reject("Remote image and release versions disagree")

    def test_path_and_package_pairing_are_refused(self):
        self.field("package_file", "../reliefos-kernel-9.0.1-r0.apk")
        self.reject("Invalid package or module directory pairing")

    def test_frozen_index_race_is_refused(self):
        index = self.tree / "serve/apk/packages.adb"
        index.write_bytes(index.read_bytes() + b"torn publication")
        self.reject("Repository checksum mismatch")

    def test_unsigned_index_is_refused_even_with_matching_transport_hash(self):
        self.reindex(signed=False)
        self.reject()

    def test_archive_checksum_failure_is_refused_before_apk_commit(self):
        archive = self.tree / "serve/apk/reliefos-kernel-9.0.1-r0.apk"
        archive.write_bytes(archive.read_bytes() + b"corrupt archive")
        self.reject("Kernel package checksum mismatch")

    def test_corrupt_module_in_new_signed_package_is_refused(self):
        payload = Path(self.temporary_case.name) / "corrupt-payload"
        shutil.copytree(self.payloads["new"], payload)
        module = payload / "lib/modules" / self.releases["new"] / "kernel/rootfs.ko"
        module.write_bytes(module.read_bytes() + b"signed but inconsistent module")
        self.repack_kernel(payload)
        self.reject("kernel-bundle:")

    def test_advertised_abi_and_signed_bundle_must_agree(self):
        self.field("module_abi", "c" * 64)
        self.reject("advertised boot identity disagree")

    def test_multiple_kernel_candidates_in_signed_index_are_refused(self):
        old = self.repositories["old"] / "reliefos-kernel-9.0.0-r0.apk"
        shutil.copy2(old, self.tree / "serve/apk" / old.name)
        self.reindex()
        self.reject("advertised kernel package disagree")

    def test_missing_signed_dependency_is_refused(self):
        (self.tree / "serve/apk/reliefos-grub-9.0.1-r0.apk").unlink()
        self.reindex()
        self.reject("Cannot resolve complete signed kernel dependencies")

    def test_legacy_loader_requires_installer_migration_before_commit(self):
        (self.tree / "boot/grub/kernel-slots.cfg").unlink()
        self.reject("matched GRUB resources")

    def test_failed_package_script_preserves_boot_and_retained_old_bundle(self):
        failure = Path(self.temporary_case.name) / "activation-failure"
        failure.write_text("#!/bin/sh\necho 'injected activation failure' >&2\nexit 1\n")
        failure.chmod(0o755)
        self.repack_kernel(self.payloads["new"], failure)
        result = self.update()
        self.assertNotEqual(result.returncode, 0, self.output)
        self.assertIn("injected activation failure", self.output)
        self.unchanged(database=False)
        result = subprocess.run([fixtures.RECOVERY, "--restore-all", self.tree], text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for name in ("old", "new"):
            result = subprocess.run([fixtures.RECOVERY, "--inspect", self.tree, self.releases[name]], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_current_package_does_not_fetch_archives_and_force_repairs_it(self):
        shutil.rmtree(self.tree / "serve/apk")
        shutil.copytree(self.repositories["old"], self.tree / "serve/apk")
        prefix = self.payloads["old"] / "usr/lib/reliefos/kernel" / self.releases["old"]
        result = subprocess.run([fixtures.KERNEL_TOOLS, "release-info", self.payloads["old"], prefix / "kernel.sys",
            self.tree / "serve/apk/reliefos-kernel-9.0.0-r0.apk", self.tree / "serve/apk/packages.adb", "5.1.2", fixtures.RECOVERY],
            check=True, text=True, capture_output=True)
        self.manifest.write_text(result.stdout)
        self.assertEqual(self.update().returncode, 0, self.output)
        self.assertIn("Kernel image is current", self.output)
        self.unchanged()
        self.assertNotIn(".apk", (self.tree / "run/fetched").read_text())
        self.assertEqual(self.update("--force").returncode, 0, self.output)
        self.assertIn("Installed kernel", self.output)
        self.assertEqual((self.tree / "etc/apk/world").read_text(), "reliefos-kernel\n")
        self.unchanged(database=False)

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
        result = subprocess.run(["sh", ROOT / "tools/build/rpr-pages.sh", repository, apps, prefix / "kernel.sys",
            self.payloads["new"], version, APK, self.key, directory / "published"], text=True, capture_output=True,
            env={**os.environ, "RELIEFOS_KERNEL_TOOLS": str(fixtures.KERNEL_TOOLS), "RELIEFOS_KERNEL_RECOVERY": str(fixtures.RECOVERY)})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        metadata = json.loads((directory / "published/kernel/release.json").read_bytes())
        self.assertEqual(metadata["schema"], 4)
        self.assertEqual(metadata["kernel_release"], self.releases["new"])


if __name__ == "__main__":
    unittest.main()
