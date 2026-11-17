#!/usr/bin/env python3
"""Exercise the native online updater with genuine signed APK transactions.

Every target is an owned temporary root. A loopback HTTP server supplies signed
APK repositories; this tests freezing, dependency resolution, cache integrity
and recovery, and intentionally makes no claim about HTTPS or CA validation.
No package solver, APK database writer or signature verifier is mocked.
"""
from __future__ import annotations

import functools
import hashlib
import http.server
import os
from pathlib import Path
import re
import signal
import time
import shutil
import subprocess
import tempfile
import threading
import unittest
from runtime_test_dependencies import crypto_flags

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = Path(os.environ.get("RELIEFOS_TEST_OUTPUT", ROOT / "out/x86_64/release")).resolve()
CLIENT = Path(os.environ.get("RELIEFOS_TEST_SYSTEM_UPDATE", OUTPUT / "userland/systemupdate.elf")).resolve()
APK = Path(os.environ.get("RELIEFOS_TEST_APK", OUTPUT / "upstream/apk/apk.static")).resolve()
BUSYBOX = Path(os.environ.get("RELIEFOS_TEST_BUSYBOX", OUTPUT / "userland/initramfs/busybox.elf")).resolve()
CLIENT_LIBRARIES = [Path(path) for path in os.environ.get("RELIEFOS_TEST_SYSTEM_UPDATE_LIBRARIES", "").split(":") if path]
PLAN_TOKEN = re.compile(r"(?m)^plan_id=([0-9a-f]{64})$")


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class RepositoryServer(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, directory: Path):
        self.requests: list[str] = []
        self.fail_fragment: str | None = None
        self.block_fragment: str | None = None
        self.entered = threading.Event()
        self.release = threading.Event()
        super().__init__(("127.0.0.1", 0), functools.partial(RepositoryHandler, directory=str(directory)))


class RepositoryHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        self.server.requests.append(self.path)
        if self.server.block_fragment and self.server.block_fragment in self.path:
            self.server.entered.set()
            if not self.server.release.wait(20):
                self.send_error(503, "owned fixture gate timed out")
                return
        if self.server.fail_fragment and self.server.fail_fragment in self.path:
            self.send_error(503, "owned fixture download failure")
            return
        try:
            super().do_GET()
        except (BrokenPipeError, ConnectionResetError):
            pass


class SystemUpdate(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        missing = [str(path) for path in (CLIENT, APK, BUSYBOX, *CLIENT_LIBRARIES) if not path.is_file()]
        if missing:
            raise unittest.SkipTest("build native updater/APK/BusyBox or set RELIEFOS_TEST_*: " + ", ".join(missing))
        cls.temporary = tempfile.TemporaryDirectory(prefix="reliefos-native-update-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.work = Path(cls.temporary.name)
        cls.policy_guard = cls.work / "policy-guard"
        subprocess.run(["cc", "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", "-static",
                        ROOT / "tools/tests/system_update_policy_guard.c",
                        *sorted(path for path in (ROOT / "userland/apps/systemupdate").glob("*.c") if path.name != "main.c"),
                        *crypto_flags(), "-o", cls.policy_guard], check=True)
        cls.keys = cls.work / "keys"
        cls.keys.mkdir()
        cls.key = cls.work / "qa.rsa"
        subprocess.run(["openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048",
                        "-out", cls.key], check=True, capture_output=True)
        cls.key.chmod(0o600)
        subprocess.run(["openssl", "pkey", "-in", cls.key, "-pubout", "-out", cls.keys / "qa.rsa.pub"],
                       check=True, capture_output=True)
        cls.old = cls.work / "old"
        cls.new = cls.work / "new"
        cls.testing = cls.work / "testing"
        for repository in (cls.old, cls.new, cls.testing):
            repository.mkdir()
        for repository, version in ((cls.old, "1.0-r0"), (cls.new, "2.0-r0")):
            cls.package(repository, "external-lib", version)
            cls.package(repository, "external-app", version, depends=["external-lib=" + version])
            cls.package(repository, "reliefos-fixture", version,
                        files={"etc/reliefos/user-settings.conf": "factory=" + version + "\n"})
        cls.package(cls.old, "pinned-app", "1.0-r0")
        cls.package(cls.new, "pinned-app", "1.0-r0")
        cls.package(cls.new, "pinned-app", "2.0-r0")
        cls.package(cls.testing, "tagged-app", "1.0-r0")
        cls.package(cls.testing, "tagged-app", "2.0-r0")
        cls.package(cls.new, "tagged-app", "3.0-r0")
        for repository in (cls.old, cls.new, cls.testing):
            cls.index(repository)
        cls.base = cls.work / "base"
        for directory in ("etc/apk/keys", "etc/apk/protected_paths.d", "etc/reliefos", "etc/ssl/certs", "lib/apk/db", "bin",
                          "sbin", "usr/sbin", "usr/libexec/reliefos", "var/lib/reliefos", "var/cache/apk", "tmp", "run", "dev",
                          "home/test"):
            (cls.base / directory).mkdir(parents=True, exist_ok=True)
        shutil.copy2(APK, cls.base / "sbin/apk")
        shutil.copy2(BUSYBOX, cls.base / "bin/busybox")
        # Optional host sanitizer runs need their real loader and libraries
        # inside the same owned chroot used by genuine APK commit hooks.
        for library in CLIENT_LIBRARIES:
            destination = cls.base / library.relative_to("/")
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(library, destination)
        (cls.base / "bin/sh").symlink_to("busybox")
        shutil.copy2(cls.keys / "qa.rsa.pub", cls.base / "etc/apk/keys/qa.rsa.pub")
        (cls.base / "etc/apk/world").write_text("")
        (cls.base / "etc/apk/arch").write_text("x86_64\n")
        (cls.base / "etc/apk/protected_paths.d/test.list").write_text("+etc/reliefos\n")
        # Match the real apk-tools package layout, including its CA link.
        subprocess.run(["openssl", "req", "-x509", "-new", "-key", cls.key, "-days", "1",
                        "-subj", "/CN=ReliefOS native updater CA fixture", "-out",
                        cls.base / "etc/ssl/certs/ca-certificates.crt"], check=True, capture_output=True)
        (cls.base / "etc/apk/ca.pem").symlink_to("../ssl/certs/ca-certificates.crt")
        old_configuration = cls.work / "old.repositories"
        old_configuration.write_text("ndx " + str(cls.old / "packages.adb") + "\n")
        cls.apk("--root", cls.base, "--keys-dir", cls.keys, "--repositories-file", old_configuration,
                "--no-network", "--no-scripts", "--initdb", "add", "reliefos-fixture", "external-app", "pinned-app=1.0-r0")
        (cls.base / "etc/reliefos/user-settings.conf").write_text("local administrator settings\n")
        (cls.base / "home/test/document.txt").write_text("用户文件必须保留\n")

    @classmethod
    def apk(cls, *arguments, success=True):
        result = subprocess.run(["unshare", "-Urmp", "--fork", APK, *map(str, arguments)],
                                capture_output=True, text=True, timeout=60)
        if success and result.returncode:
            raise AssertionError("APK failed: " + result.stdout + result.stderr)
        return result

    @classmethod
    def package(cls, repository: Path, name: str, version: str, depends=(), files=None, script=None, executables=()):
        with tempfile.TemporaryDirectory(prefix="payload-", dir=cls.work) as temporary:
            payload = Path(temporary)
            all_files = {"usr/share/" + name + "/version": version + "\n", **(files or {})}
            for member, contents in all_files.items():
                destination = payload / member
                destination.parent.mkdir(parents=True, exist_ok=True)
                if isinstance(contents, bytes):
                    destination.write_bytes(contents)
                else:
                    destination.write_text(contents)
                if member in executables:
                    destination.chmod(0o755)
            arguments = ["mkpkg", "--files", payload, "--output", repository / (name + "-" + version + ".apk"),
                         "--info", "name:" + name, "--info", "version:" + version, "--info", "arch:x86_64",
                         "--sign-key", cls.key]
            if depends:
                arguments += ["--info", "depends:" + " ".join(depends)]
            if script:
                arguments += ["--script", "post-install:" + str(script), "--script", "post-upgrade:" + str(script)]
            cls.apk(*arguments)

    @classmethod
    def index(cls, repository: Path, signed=True):
        arguments = ["mkndx", "--keys-dir", cls.keys, "--output", repository / "packages.adb"]
        arguments += ["--sign-key", cls.key] if signed else ["--allow-untrusted"]
        cls.apk(*arguments, *sorted(repository.glob("*.apk")))

    def setUp(self):
        self.temporary_case = tempfile.TemporaryDirectory(prefix="case-", dir=self.work)
        self.addCleanup(self.temporary_case.cleanup)
        self.case = Path(self.temporary_case.name)
        self.root = self.case / "root"
        shutil.copytree(self.base, self.root, symlinks=True)
        shutil.copy2(CLIENT, self.root / "usr/sbin/reliefos-update")
        (self.root / "lib/apk/commit_hooks.d").mkdir()
        (self.root / "lib/apk/commit_hooks.d/reliefos-update").symlink_to("/usr/sbin/reliefos-update")
        self.repository = self.case / "serve/main"
        shutil.copytree(self.new, self.repository)
        shutil.copytree(self.testing, self.case / "serve/testing")
        self.server = RepositoryServer(self.case / "serve")
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.addCleanup(self.stop_server)
        self.url = "http://127.0.0.1:" + str(self.server.server_port)
        (self.root / "etc/apk/repositories").write_text("ndx " + self.url + "/main/packages.adb\n")
        self.output = ""

    def stop_server(self):
        if self.server is not None:
            self.server.release.set()
            self.server.shutdown()
            self.server.server_close()
            self.thread.join(timeout=5)
            self.server = None

    def command(self, *arguments):
        return ["unshare", "-Urm", "--fork", str(CLIENT), "--root", str(self.root), *map(str, arguments)]

    def update(self, *arguments, timeout=90):
        result = subprocess.run(self.command(*arguments), capture_output=True, text=True, timeout=timeout)
        self.output = result.stdout + result.stderr
        self.assertNotRegex(self.output, r"AddressSanitizer:|LeakSanitizer:|runtime error:")
        return result

    def snapshot(self):
        ignored = ("var/lib/reliefos/update/", "var/cache/reliefos/update/", "var/log/", "run/", "tmp/")
        return {str(path.relative_to(self.root)): sha(path) for path in self.root.rglob("*")
                if path.is_file() and not path.is_symlink() and
                not str(path.relative_to(self.root)).startswith(ignored)}

    def prepare(self):
        before = self.snapshot()
        result = self.update("prepare", "--machine")
        self.assertEqual(result.returncode, 0, self.output)
        tokens = PLAN_TOKEN.findall(result.stdout)
        self.assertEqual(len(tokens), 1, self.output)
        self.assertEqual(self.snapshot(), before, self.output)
        return tokens[0]

    def apply(self, plan):
        return self.update("apply", "--plan", plan, "--machine")

    def assert_rejected_without_package_changes(self, plan):
        before = self.snapshot()
        result = self.apply(plan)
        self.assertNotEqual(result.returncode, 0, self.output)
        self.assertEqual(self.snapshot(), before, self.output)

    def test_complete_signed_system_and_upstream_upgrade_preserves_local_files_and_pins(self):
        world = (self.root / "etc/apk/world").read_bytes()
        repositories = (self.root / "etc/apk/repositories").read_bytes()
        plan = self.prepare()
        result = self.apply(plan)
        self.assertEqual(result.returncode, 0, self.output)
        for package in ("reliefos-fixture", "external-app", "external-lib"):
            self.assertEqual((self.root / "usr/share" / package / "version").read_text(), "2.0-r0\n")
        self.assertEqual((self.root / "usr/share/pinned-app/version").read_text(), "1.0-r0\n")
        self.assertEqual((self.root / "etc/apk/world").read_bytes(), world)
        self.assertEqual((self.root / "etc/apk/repositories").read_bytes(), repositories)
        self.assertEqual((self.root / "home/test/document.txt").read_text(), "用户文件必须保留\n")
        self.assertEqual((self.root / "etc/reliefos/user-settings.conf").read_text(), "local administrator settings\n")

    def test_standard_ca_link_is_frozen_and_preserved_after_upgrade(self):
        link = self.root / "etc/apk/ca.pem"
        bundle = self.root / "etc/ssl/certs/ca-certificates.crt"
        plan = self.prepare()
        frozen = self.root / "var/lib/reliefos/update/plans" / plan / "snapshot/etc/apk/ca.pem"
        self.assertFalse(frozen.is_symlink())
        self.assertEqual(frozen.read_bytes(), bundle.read_bytes())
        self.assertEqual(frozen.stat().st_mode & 0o777, 0o600)
        result = self.apply(plan)
        self.assertEqual(result.returncode, 0, self.output)
        self.assertTrue(link.is_symlink())
        self.assertEqual(os.readlink(link), "../ssl/certs/ca-certificates.crt")
        self.assertEqual((self.root / "usr/share/external-app/version").read_text(), "2.0-r0\n")

    def test_absolute_standard_ca_link_uses_owned_root_bundle(self):
        link = self.root / "etc/apk/ca.pem"
        link.unlink()
        link.symlink_to("/etc/ssl/certs/ca-certificates.crt")
        plan = self.prepare()
        frozen = self.root / "var/lib/reliefos/update/plans" / plan / "snapshot/etc/apk/ca.pem"
        self.assertEqual(frozen.read_bytes(), (self.root / "etc/ssl/certs/ca-certificates.crt").read_bytes())
        result = self.apply(plan)
        self.assertEqual(result.returncode, 0, self.output)
        self.assertEqual(os.readlink(link), "/etc/ssl/certs/ca-certificates.crt")

    def test_ca_bundle_change_rejects_stale_plan(self):
        plan = self.prepare()
        with (self.root / "etc/ssl/certs/ca-certificates.crt").open("a") as stream:
            stream.write("\n# administrator changed CA trust\n")
        self.assert_rejected_without_package_changes(plan)
        self.assertIn("prepare a new plan", self.output)

    def test_ca_link_identity_change_rejects_stale_plan(self):
        plan = self.prepare()
        link = self.root / "etc/apk/ca.pem"
        link.unlink()
        link.symlink_to("/etc/ssl/certs/ca-certificates.crt")
        self.assert_rejected_without_package_changes(plan)

    def test_noncanonical_ca_links_are_rejected_before_network(self):
        link = self.root / "etc/apk/ca.pem"
        for target in ("../../../../etc/passwd", "/etc/passwd", "../ssl/certs/./ca-certificates.crt"):
            with self.subTest(target=target):
                link.unlink()
                link.symlink_to(target)
                before = self.snapshot()
                result = self.update("prepare", "--machine")
                self.assertNotEqual(result.returncode, 0, self.output)
                self.assertEqual(self.snapshot(), before, self.output)
                self.assertEqual(self.server.requests, [])

    def test_other_configuration_symlinks_remain_rejected(self):
        (self.root / "etc/apk/not-ca.pem").symlink_to("../ssl/certs/ca-certificates.crt")
        before = self.snapshot()
        result = self.update("prepare", "--machine")
        self.assertNotEqual(result.returncode, 0, self.output)
        self.assertEqual(self.snapshot(), before, self.output)
        self.assertEqual(self.server.requests, [])

    def test_nested_ca_named_symlinks_remain_rejected(self):
        nested = self.root / "etc/apk/keys/nested/etc"
        (nested / "apk").mkdir(parents=True)
        (nested / "ssl/certs").mkdir(parents=True)
        (nested / "ssl/certs/ca-certificates.crt").write_bytes(
            (self.root / "etc/ssl/certs/ca-certificates.crt").read_bytes())
        (nested / "apk/ca.pem").symlink_to("../ssl/certs/ca-certificates.crt")
        before = self.snapshot()
        result = self.update("prepare", "--machine")
        self.assertNotEqual(result.returncode, 0, self.output)
        self.assertEqual(self.snapshot(), before, self.output)
        self.assertEqual(self.server.requests, [])

    def test_canonical_ca_bundle_cannot_redirect_through_another_link(self):
        bundle = self.root / "etc/ssl/certs/ca-certificates.crt"
        bundle.unlink()
        bundle.symlink_to("/etc/ssl/cert.pem")
        before = self.snapshot()
        result = self.update("prepare", "--machine")
        self.assertNotEqual(result.returncode, 0, self.output)
        self.assertEqual(self.snapshot(), before, self.output)
        self.assertEqual(self.server.requests, [])

    def test_reused_pid_temporary_remnant_cannot_block_persistent_state(self):
        base = self.root / "var/lib/reliefos/update"
        base.mkdir(mode=0o700)
        # Reproduce an orphan from an earlier process with the exact PID of
        # the next client. A reboot can reuse a PID while retaining this file.
        script = ('printf "owned interrupted write\\n" > "$1/var/lib/reliefos/update/.new-$$-active"; '
                  'exec "$2" --root "$1" prepare --machine')
        result = subprocess.run(["unshare", "-Urm", "--fork", "/bin/sh", "-c", script,
                                 "updater-remnant", self.root, CLIENT],
                                capture_output=True, text=True, timeout=90)
        self.output = result.stdout + result.stderr
        self.assertNotRegex(self.output, r"AddressSanitizer:|LeakSanitizer:|runtime error:")
        self.assertEqual(result.returncode, 0, self.output)
        tokens = PLAN_TOKEN.findall(result.stdout)
        self.assertEqual(len(tokens), 1, self.output)
        remnants = list(base.glob(".new-*-active"))
        self.assertEqual(len(remnants), 1)
        self.assertEqual(remnants[0].read_text(), "owned interrupted write\n")
        self.assertEqual((base / "active").read_text().strip(), tokens[0])
        result = self.apply(tokens[0])
        self.assertEqual(result.returncode, 0, self.output)
        self.assertTrue(remnants[0].exists(), "recovery must retain unrelated interrupted-write evidence")

    def test_apply_uses_frozen_cache_when_repository_goes_offline(self):
        plan = self.prepare()
        self.stop_server()
        result = self.apply(plan)
        self.assertEqual(result.returncode, 0, self.output)
        self.assertEqual((self.root / "usr/share/external-app/version").read_text(), "2.0-r0\n")

    def test_resume_ready_returns_plan_without_installing_packages(self):
        plan = self.prepare()
        before = self.snapshot()
        result = self.update("resume", "--machine")
        self.assertEqual(result.returncode, 0, self.output)
        self.assertEqual(PLAN_TOKEN.findall(result.stdout), [plan], self.output)
        self.assertEqual(self.snapshot(), before, self.output)

    def test_discard_ready_preserves_evidence_and_allows_a_new_plan(self):
        plan = self.prepare()
        before = self.snapshot()
        evidence = self.root / "var/lib/reliefos/update/plans" / plan / "cache"
        result = self.update("discard", "--machine")
        self.assertEqual(result.returncode, 0, self.output)
        self.assertEqual(self.snapshot(), before, self.output)
        self.assertTrue(any(evidence.glob("*.apk")))
        result = self.update("status", "--machine")
        self.assertEqual(result.returncode, 0, self.output)
        self.assertIn("phase=stale", result.stdout)
        self.assertNotEqual(self.prepare(), plan)

    def test_discard_failed_download_allows_a_new_plan(self):
        self.server.fail_fragment = "external-lib-2.0-r0"
        before = self.snapshot()
        result = self.update("prepare", "--machine")
        self.assertNotEqual(result.returncode, 0, self.output)
        result = self.update("discard", "--machine")
        self.assertEqual(result.returncode, 0, self.output)
        self.assertEqual(self.snapshot(), before, self.output)
        self.server.fail_fragment = None
        self.prepare()

    def test_apply_does_not_fetch_new_publication_after_prepare(self):
        plan = self.prepare()
        self.package(self.repository, "reliefos-fixture", "3.0-r0")
        self.index(self.repository)
        requests_before = len(self.server.requests)
        result = self.apply(plan)
        self.assertEqual(result.returncode, 0, self.output)
        self.assertEqual(len(self.server.requests), requests_before, self.server.requests)
        self.assertEqual((self.root / "usr/share/reliefos-fixture/version").read_text(), "2.0-r0\n")

    def test_world_change_rejects_stale_plan(self):
        plan = self.prepare()
        with (self.root / "etc/apk/world").open("a") as stream:
            stream.write("external-lib\n")
        self.assert_rejected_without_package_changes(plan)

    def test_database_change_rejects_stale_plan(self):
        plan = self.prepare()
        with (self.root / "lib/apk/db/installed").open("a") as stream:
            stream.write("\n")
        self.assert_rejected_without_package_changes(plan)

    def test_repository_change_rejects_stale_plan(self):
        plan = self.prepare()
        with (self.root / "etc/apk/repositories").open("a") as stream:
            stream.write("# administrator changed repository configuration\n")
        self.assert_rejected_without_package_changes(plan)

    def test_repository_dropin_change_rejects_stale_plan(self):
        directory = self.root / "etc/apk/repositories.d"
        directory.mkdir()
        path = directory / "testing.list"
        path.write_text("ndx @testing " + self.url + "/testing/packages.adb\n")
        plan = self.prepare()
        path.write_text("# testing disabled by administrator\n")
        self.assert_rejected_without_package_changes(plan)

    def test_vendor_repository_dropin_change_rejects_stale_plan(self):
        directory = self.root / "lib/apk/repositories.d"
        directory.mkdir()
        path = directory / "testing.list"
        path.write_text("ndx @testing " + self.url + "/testing/packages.adb\n")
        plan = self.prepare()
        path.write_text("# vendor source changed\n")
        self.assert_rejected_without_package_changes(plan)

    def test_administrator_dropin_shadows_same_named_vendor_file(self):
        administrator = self.root / "etc/apk/repositories.d"
        vendor = self.root / "lib/apk/repositories.d"
        administrator.mkdir()
        vendor.mkdir()
        (administrator / "vendor.list").write_text("# administrator disabled this vendor source\n")
        (vendor / "vendor.list").write_text("ndx http://127.0.0.1:1/disabled.adb\n")
        plan = self.prepare()
        result = self.apply(plan)
        self.assertEqual(result.returncode, 0, self.output)

    def test_standard_dropin_only_repository_configuration(self):
        main = self.root / "etc/apk/repositories"
        contents = main.read_bytes()
        main.unlink()
        directory = self.root / "etc/apk/repositories.d"
        directory.mkdir()
        (directory / "main.list").write_bytes(contents)
        plan = self.prepare()
        result = self.apply(plan)
        self.assertEqual(result.returncode, 0, self.output)
        self.assertFalse(main.exists(), "updater must preserve a dropin-only administrator configuration")

    def test_trust_key_change_rejects_stale_plan(self):
        plan = self.prepare()
        (self.root / "etc/apk/keys/new-key.rsa.pub").write_bytes((self.keys / "qa.rsa.pub").read_bytes())
        self.assert_rejected_without_package_changes(plan)

    def install_rpr_fixture_transport(self):
        # Deterministic metadata-only transport fixture. Native APK still
        # verifies publisher signatures; this helper does not test HTTPS.
        source = self.root / "qa-rpr"
        shutil.copytree(self.repository, source / "apk")
        (source / "health.txt").write_text("leonos-rpr-ok\n")
        (self.root / "etc/reliefos/rpr.conf").write_text("RPR_BASE_URL=https://fixture.invalid/rpr\n")
        transport = self.root / "usr/sbin/rprfetch"
        transport.write_text(
            "#!/bin/sh\nset -eu\n"
            "case $1 in https://fixture.invalid/rpr/*) ;; *) exit 2 ;; esac\n"
            "relative=${1#https://fixture.invalid/rpr/}\n"
            "case $relative in apk/packages.adb|health.txt) ;; *) exit 3 ;; esac\n"
            "printf '%s\\n' \"$relative\" >> '" + str(self.root / "run/rpr-requests") + "'\n"
            "cp '" + str(source) + "/'\"$relative\" \"$2\"\n")
        transport.chmod(0o755)
        return source

    def test_configure_preserves_existing_key_and_is_idempotent(self):
        self.install_rpr_fixture_transport()
        trusted = {path.name: path.read_bytes() for path in (self.root / "etc/apk/keys").iterdir()}
        for _ in range(2):
            result = self.update("configure", "--machine")
            self.assertEqual(result.returncode, 0, self.output)
        self.assertEqual({path.name: path.read_bytes() for path in (self.root / "etc/apk/keys").iterdir()}, trusted)
        entries = (self.root / "etc/apk/repositories").read_text().splitlines()
        self.assertEqual(entries.count("ndx https://fixture.invalid/rpr/apk/packages.adb"), 1)
        self.assertNotIn(".pub", (self.root / "run/rpr-requests").read_text())

    def test_untrusted_publisher_key_is_not_imported_by_configure(self):
        source = self.install_rpr_fixture_transport()
        foreign = self.case / "foreign.rsa"
        subprocess.run(["openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048",
                        "-out", foreign], check=True, capture_output=True)
        foreign.chmod(0o600)
        subprocess.run(["openssl", "pkey", "-in", foreign, "-pubout", "-out", source / "apk/reliefos-rpr.rsa.pub"],
                       check=True, capture_output=True)
        self.apk("mkndx", "--keys-dir", self.keys, "--sign-key", foreign, "--output", source / "apk/packages.adb",
                 *sorted((source / "apk").glob("*.apk")))
        trusted = {path.name: path.read_bytes() for path in (self.root / "etc/apk/keys").iterdir()}
        repositories = (self.root / "etc/apk/repositories").read_bytes()
        result = self.update("configure", "--machine")
        self.assertNotEqual(result.returncode, 0, self.output)
        self.assertEqual({path.name: path.read_bytes() for path in (self.root / "etc/apk/keys").iterdir()}, trusted)
        self.assertEqual((self.root / "etc/apk/repositories").read_bytes(), repositories)
        self.assertNotIn(".pub", (self.root / "run/rpr-requests").read_text())

    def test_unsigned_repository_is_rejected_before_package_changes(self):
        self.index(self.repository, signed=False)
        before = self.snapshot()
        result = self.update("prepare", "--machine")
        self.assertNotEqual(result.returncode, 0, self.output)
        self.assertFalse(PLAN_TOKEN.search(result.stdout), self.output)
        self.assertEqual(self.snapshot(), before, self.output)

    def test_inherited_apk_config_cannot_disable_signature_checks(self):
        configuration = self.case / "unsafe-apk-config"
        configuration.write_text("allow-untrusted\nno-commit-hooks\n")
        self.index(self.repository, signed=False)
        polluted_environment = {**os.environ, "APK_CONFIG": str(configuration)}
        unsafe = subprocess.run([APK, "verify", self.repository / "packages.adb"],
                                capture_output=True, text=True, env=polluted_environment, timeout=30)
        self.assertEqual(unsafe.returncode, 0, "fixture must actually bypass APK verification: " + unsafe.stdout + unsafe.stderr)
        before = self.snapshot()
        result = subprocess.run(self.command("prepare", "--machine"), capture_output=True, text=True,
                                env=polluted_environment, timeout=90)
        self.output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, self.output)
        self.assertNotRegex(self.output, r"AddressSanitizer:|LeakSanitizer:|runtime error:")
        self.assertFalse(PLAN_TOKEN.search(result.stdout), self.output)
        self.assertEqual(self.snapshot(), before, self.output)

    def test_corrupt_archive_is_rejected_before_package_changes(self):
        archive = self.repository / "external-lib-2.0-r0.apk"
        contents = bytearray(archive.read_bytes())
        contents[len(contents) // 2] ^= 1
        archive.write_bytes(contents)
        before = self.snapshot()
        result = self.update("prepare", "--machine")
        self.assertNotEqual(result.returncode, 0, self.output)
        self.assertFalse(PLAN_TOKEN.search(result.stdout), self.output)
        self.assertEqual(self.snapshot(), before, self.output)

    def test_corrupt_verified_cache_is_rejected_before_package_changes(self):
        plan = self.prepare()
        cache = self.root / "var/lib/reliefos/update/plans" / plan / "cache"
        archives = sorted(cache.glob("*.apk"))
        self.assertTrue(archives, "prepared update must own downloaded archives")
        data = bytearray(archives[0].read_bytes())
        data[len(data) // 2] ^= 1
        archives[0].write_bytes(data)
        self.assert_rejected_without_package_changes(plan)

    def test_plan_path_traversal_and_noncanonical_tokens_are_rejected(self):
        for token in ("../outside", "a" * 63, "A" * 64, "0" * 64 + "/file"):
            with self.subTest(token=token):
                self.assert_rejected_without_package_changes(token)

    def test_failed_download_can_resume_from_persistent_state(self):
        self.server.fail_fragment = "external-lib-2.0-r0"
        before = self.snapshot()
        result = self.update("prepare", "--machine")
        self.assertNotEqual(result.returncode, 0, self.output)
        self.assertFalse(PLAN_TOKEN.search(result.stdout), self.output)
        self.assertEqual(self.snapshot(), before, self.output)
        self.server.fail_fragment = None
        result = self.update("resume", "--machine")
        self.assertEqual(result.returncode, 0, self.output)
        tokens = PLAN_TOKEN.findall(result.stdout)
        self.assertEqual(len(tokens), 1, self.output)
        result = self.apply(tokens[0])
        self.assertEqual(result.returncode, 0, self.output)

    def test_explicit_tagged_world_request_keeps_repository_policy(self):
        with (self.root / "etc/apk/repositories").open("a") as stream:
            stream.write("ndx @testing " + self.url + "/testing/packages.adb\n")
        self.apk("--root", self.root, "--keys-dir", self.root / "etc/apk/keys", "--repositories-file",
                 self.root / "etc/apk/repositories", "update")
        self.apk("--root", self.root, "--keys-dir", self.root / "etc/apk/keys", "--repositories-file",
                 self.root / "etc/apk/repositories", "--no-scripts", "add", "tagged-app@testing=1.0-r0")
        path = self.root / "etc/apk/world"
        path.write_text(path.read_text().replace("tagged-app@testing=1.0-r0", "tagged-app@testing"))
        world = path.read_bytes()
        expected = self.case / "expected-policy-root"
        shutil.copytree(self.root, expected, symlinks=True)
        self.apk("--root", expected, "--keys-dir", expected / "etc/apk/keys", "--repositories-file",
                 expected / "etc/apk/repositories", "update")
        self.apk("--root", expected, "--keys-dir", expected / "etc/apk/keys", "--repositories-file",
                 expected / "etc/apk/repositories", "--no-scripts", "--no-commit-hooks", "upgrade", "--no-self-upgrade")
        expected_version = (expected / "usr/share/tagged-app/version").read_bytes()
        plan = self.prepare()
        result = self.apply(plan)
        self.assertEqual(result.returncode, 0, self.output)
        self.assertEqual(path.read_bytes(), world)
        # APK tags permit default repositories too: compare its real solver's
        # result, rather than inventing a different repository-priority policy.
        self.assertEqual((self.root / "usr/share/tagged-app/version").read_bytes(), expected_version)

    def test_package_script_failure_is_recorded_and_repair_failure_is_not_success(self):
        script = self.case / "fail-post-upgrade"
        script.write_text("#!/bin/sh\necho 'owned fixture post-upgrade failure' >&2\nexit 17\n")
        script.chmod(0o755)
        self.package(self.repository, "reliefos-fixture", "3.0-r0", script=script)
        self.index(self.repository)
        plan = self.prepare()
        result = self.apply(plan)
        self.assertNotEqual(result.returncode, 0, self.output)
        self.assertIn("owned fixture post-upgrade failure", self.output)
        result = self.update("status", "--machine")
        self.assertEqual(result.returncode, 0, self.output)
        self.assertRegex(self.output.lower(), r"failed|repair|interrupted")
        result = self.update("repair", "--machine")
        self.assertNotEqual(result.returncode, 0, self.output)

    def failed_commit(self):
        script = self.case / "fail-before-repair"
        script.write_text("#!/bin/sh\nexit 17\n")
        script.chmod(0o755)
        self.package(self.repository, "reliefos-fixture", "3.0-r0", script=script)
        self.index(self.repository)
        plan = self.prepare()
        result = self.apply(plan)
        self.assertNotEqual(result.returncode, 0, self.output)
        return plan

    def assert_repair_rejects_changed_configuration(self):
        before = self.snapshot()
        result = self.update("repair", "--machine")
        self.assertNotEqual(result.returncode, 0, self.output)
        self.assertEqual(self.snapshot(), before, self.output)
        self.assertNotIn("Executing reliefos-fixture", self.output)

    def test_repair_rejects_world_changed_after_failed_commit(self):
        self.failed_commit()
        with (self.root / "etc/apk/world").open("a") as stream:
            stream.write("external-lib\n")
        self.assert_repair_rejects_changed_configuration()

    def test_repair_rechecks_policy_after_space_preflight(self):
        # Delegate every operation to the real static APK, changing only an
        # administrator-owned world file at the exact read-only size boundary.
        real_apk = self.root / "sbin/apk.real"
        shutil.copy2(APK, real_apk)
        gate = self.root / "run/change-world-during-size-query"
        world = self.root / "etc/apk/world"
        wrapper = self.root / "sbin/apk"
        wrapper.write_text("#!/bin/sh\nset -eu\nquery=0\n"
            "for value do [ \"$value\" != repositories ] || query=1; done\n"
            "if [ \"$query\" = 1 ] && [ -f '" + str(gate) + "' ]; then\n"
            "    printf 'external-lib\\n' >> '" + str(world) + "'\n"
            "    rm '" + str(gate) + "'\nfi\nexec '" + str(real_apk) + "' \"$@\"\n")
        wrapper.chmod(0o755)
        self.failed_commit()
        database = (self.root / "lib/apk/db/installed").read_bytes()
        gate.write_text("change policy only after initial repair validation\n")
        result = self.update("repair", "--machine")
        self.assertNotEqual(result.returncode, 0, self.output)
        self.assertFalse(gate.exists(), "fixture must actually enter the size preflight window")
        self.assertTrue(world.read_text().endswith("external-lib\n"))
        self.assertIn("world, repository policy or trust changed", self.output)
        self.assertNotIn("event=repairing-packages", self.output)
        self.assertEqual((self.root / "lib/apk/db/installed").read_bytes(), database)

    def test_repair_rechecks_policy_after_reinstall_scripts(self):
        script = self.case / "repair-policy-change"
        script.write_text("#!/bin/sh\n[ -f /run/allow-policy-change ] || exit 17\n"
                          "printf '# changed during signed reinstall\\n' >> /etc/apk/repositories\n")
        script.chmod(0o755)
        self.package(self.repository, "reliefos-fixture", "3.0-r0", script=script)
        self.index(self.repository)
        plan = self.prepare()
        self.assertNotEqual(self.apply(plan).returncode, 0, self.output)
        (self.root / "run/allow-policy-change").write_text("exercise signed reinstall script\n")
        result = self.update("repair", "--machine")
        self.assertNotEqual(result.returncode, 0, self.output)
        self.assertTrue((self.root / "etc/apk/repositories").read_text().endswith("# changed during signed reinstall\n"))
        self.assertIn("world, repository policy or trust changed", self.output)
        self.assertNotIn("result=success", self.output)

    def test_commit_guard_checks_frozen_policy_even_after_target_refresh(self):
        plan = self.failed_commit()
        state = self.root / "var/lib/reliefos/update/plans" / plan / "state"
        before = dict(line.split("=", 1) for line in state.read_text().splitlines())
        with (self.root / "etc/apk/world").open("a") as stream:
            stream.write("external-lib\n")
        result = subprocess.run(["unshare", "-Urm", "--fork", self.policy_guard, self.root],
                                text=True, capture_output=True, timeout=20)
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn("world, repository policy or trust changed", output)
        after = dict(line.split("=", 1) for line in state.read_text().splitlines())
        self.assertNotEqual(after["target"], before["target"], "probe must really adopt the changed target")
        self.assertEqual(after["policy"], before["policy"])

    def test_discard_refuses_a_failed_package_commit(self):
        self.failed_commit()
        before = self.snapshot()
        result = self.update("discard", "--machine")
        self.assertNotEqual(result.returncode, 0, self.output)
        self.assertEqual(self.snapshot(), before, self.output)
        result = self.update("status", "--machine")
        self.assertEqual(result.returncode, 0, self.output)
        self.assertIn("phase=apply-failed", result.stdout)

    def test_repair_rejects_trust_changed_after_failed_commit(self):
        self.failed_commit()
        (self.root / "etc/apk/keys/new-key.rsa.pub").write_bytes((self.keys / "qa.rsa.pub").read_bytes())
        self.assert_repair_rejects_changed_configuration()

    def test_repair_rejects_repository_changed_after_failed_commit(self):
        self.failed_commit()
        with (self.root / "etc/apk/repositories").open("a") as stream:
            stream.write("# administrator changed source policy after failure\n")
        self.assert_repair_rejects_changed_configuration()

    def test_saved_recovery_tools_work_when_installed_commands_are_unavailable(self):
        script = self.case / "repair-gated-post-upgrade"
        script.write_text("#!/bin/sh\n[ -f /run/fixture-allow-repair ] || exit 17\n")
        script.chmod(0o755)
        self.package(self.repository, "reliefos-fixture", "3.0-r0", script=script)
        self.index(self.repository)
        plan = self.prepare()
        result = self.apply(plan)
        self.assertNotEqual(result.returncode, 0, self.output)
        recovery = self.root / "var/lib/reliefos/update/plans" / plan / "tools/reliefos-update"
        saved_apk = recovery.parent / "apk.static"
        self.assertTrue(recovery.is_file(), "transaction must preserve a runnable recovery client")
        self.assertTrue(saved_apk.is_file(), "transaction must preserve the APK used by its plan")
        (self.root / "run/fixture-allow-repair").write_text("administrator authorized repair\n")
        (self.root / "sbin/apk").write_text("damaged installed APK\n")
        (self.root / "sbin/apk").chmod(0o755)
        (self.root / "usr/sbin/reliefos-update").unlink()
        self.stop_server()
        result = subprocess.run(["unshare", "-Urm", "--fork", recovery, "--root", self.root,
                                 "repair", "--machine"], capture_output=True, text=True, timeout=90)
        self.output = result.stdout + result.stderr
        self.assertNotRegex(self.output, r"AddressSanitizer:|LeakSanitizer:|runtime error:")
        self.assertEqual(result.returncode, 0, self.output)
        self.assertIn("result=success", result.stdout)
        self.assertEqual(sha(self.root / "sbin/apk"), sha(saved_apk))
        self.assertEqual(sha(self.root / "usr/sbin/reliefos-update"), sha(recovery))
        self.assertEqual((self.root / "usr/share/reliefos-fixture/version").read_text(), "3.0-r0\n")

    def test_reboot_pid_reuse_does_not_collide_with_interrupted_temporary_record(self):
        base = self.root / "var/lib/reliefos/update"
        base.mkdir(mode=0o700)

        def leave_interrupted_record():
            (base / (".new-" + str(os.getpid()) + "-active")).write_text("unsynced previous boot record\n")

        result = subprocess.run(self.command("prepare", "--machine"), preexec_fn=leave_interrupted_record,
                                text=True, capture_output=True, timeout=90)
        self.output = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, self.output)
        plans = PLAN_TOKEN.findall(result.stdout)
        self.assertEqual(len(plans), 1, self.output)
        leftovers = list(base.glob(".new-*-active"))
        self.assertEqual(len(leftovers), 1)
        self.assertEqual(leftovers[0].read_text(), "unsynced previous boot record\n")
        self.assertEqual(self.apply(plans[0]).returncode, 0, self.output)

    def test_repair_preserves_upgraded_apk_owned_recovery_executables(self):
        self.check_apk_owned_recovery_executables(False)

    def test_repair_reinstalls_damaged_apk_owned_recovery_executables(self):
        self.check_apk_owned_recovery_executables(True)

    def test_repair_restores_unchanged_apk_owned_recovery_executables_offline(self):
        self.check_apk_owned_recovery_executables(True, upgrade=False)

    def check_apk_owned_recovery_executables(self, damage, upgrade=True):
        old_repository = self.case / "old-tools"
        old_repository.mkdir()
        tool_files = {"reliefos-apk-tools": ("sbin/apk", APK.read_bytes()),
                      "reliefos-system-update": ("usr/sbin/reliefos-update", CLIENT.read_bytes())}
        expected = {}
        for name, (member, executable) in tool_files.items():
            old_bytes = executable + b"\nowned recovery fixture version 1\n"
            new_bytes = executable + b"\nowned recovery fixture version 2\n" if upgrade else old_bytes
            self.package(old_repository, name, "1.0-r0", files={member: old_bytes}, executables=[member])
            self.package(self.repository, name, "2.0-r0" if upgrade else "1.0-r0",
                         files={member: new_bytes}, executables=[member])
            expected[member] = new_bytes
            (self.root / member).unlink()
        self.apk("--root", self.root, "--keys-dir", self.keys, "--repositories-file", "/dev/null",
                 "--no-scripts", "--no-commit-hooks", "add", *sorted(old_repository.glob("*.apk")))
        world = self.root / "etc/apk/world"
        world.write_text("".join(next((name for name in tool_files if line.startswith(name + "><")), line) + "\n"
                                 for line in world.read_text().splitlines()))
        script = self.case / "fail-after-tool-upgrades"
        script.write_text("#!/bin/sh\n[ -f /run/fixture-allow-repair ] || exit 17\n")
        script.chmod(0o755)
        self.package(self.repository, "reliefos-fixture", "3.0-r0", script=script,
                     depends=[name + (">=2.0-r0" if upgrade else ">=1.0-r0") for name in tool_files])
        self.index(self.repository)
        plan = self.prepare()
        result = self.apply(plan)
        self.assertNotEqual(result.returncode, 0, self.output)
        for member, contents in expected.items():
            self.assertEqual((self.root / member).read_bytes(), contents,
                             "fixture must fail after the tools have actually been upgraded: " + self.output)
        if damage:
            for member in expected:
                (self.root / member).write_text("damaged upgraded executable\n")
                (self.root / member).chmod(0o755)
        (self.root / "run/fixture-allow-repair").write_text("administrator authorized repair\n")
        self.stop_server()
        if damage:
            recovery = self.root / "var/lib/reliefos/update/plans" / plan / "tools/reliefos-update"
            result = subprocess.run(["unshare", "-Urm", "--fork", recovery, "--root", self.root,
                                     "repair", "--machine"], capture_output=True, text=True, timeout=90)
            self.output = result.stdout + result.stderr
            self.assertNotRegex(self.output, r"AddressSanitizer:|LeakSanitizer:|runtime error:")
        else:
            result = self.update("repair", "--machine")
        self.assertEqual(result.returncode, 0, self.output)
        for name, (member, _) in tool_files.items():
            self.assertEqual((self.root / member).read_bytes(), expected[member],
                             "repair must leave the archive version recorded in APK, not its old bootstrap executable")
            self.assertEqual((self.root / "usr/share" / name / "version").read_text(),
                             "2.0-r0\n" if upgrade else "1.0-r0\n")

    def test_interrupted_commit_repairs_from_frozen_cache_offline(self):
        script = self.case / "block-post-upgrade"
        script.write_text(
            "#!/bin/sh\nif [ ! -f /run/fixture-allow-commit ]; then\n"
            "    printf '%s\\n' \"$$\" > /run/fixture-script.pid\n"
            "    read fixture_reply < /run/fixture-gate\nfi\n")
        script.chmod(0o755)
        os.mkfifo(self.root / "run/fixture-gate")
        self.package(self.repository, "reliefos-fixture", "3.0-r0", script=script)
        self.index(self.repository)
        plan = self.prepare()
        world = (self.root / "etc/apk/world").read_bytes()
        process = subprocess.Popen(self.command("apply", "--plan", plan, "--machine"),
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
        script_pid = None
        try:
            marker = self.root / "run/fixture-script.pid"
            deadline = time.monotonic() + 20
            while not marker.exists() and process.poll() is None and time.monotonic() < deadline:
                threading.Event().wait(0.05)
            self.assertTrue(marker.exists(), "signed script never reached controlled interruption point")
            script_pid = int(marker.read_text())
            os.killpg(process.pid, signal.SIGKILL)
            (self.root / "run/fixture-allow-commit").write_text("fixture may finish on explicit repair\n")
            try:
                gate = os.open(self.root / "run/fixture-gate", os.O_WRONLY | os.O_NONBLOCK)
                os.write(gate, b"release\n")
                os.close(gate)
            except OSError:
                pass  # Parent-death handling may already have terminated the script.
            process.communicate(timeout=15)
            self.stop_server()
            result = self.update("status", "--machine")
            self.assertEqual(result.returncode, 0, self.output)
            self.assertRegex(self.output.lower(), r"committing|failed|repair|interrupted")
            result = self.update("repair", "--machine")
            self.assertEqual(result.returncode, 0, self.output)
            self.assertEqual((self.root / "usr/share/reliefos-fixture/version").read_text(), "3.0-r0\n")
            self.assertEqual((self.root / "etc/apk/world").read_bytes(), world)
            self.assertEqual((self.root / "home/test/document.txt").read_text(), "用户文件必须保留\n")
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
            if script_pid is not None and os.path.realpath("/proc/" + str(script_pid) + "/root") == str(self.root):
                try:
                    os.kill(script_pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            process.communicate(timeout=10)

    def test_commit_guard_closes_target_change_before_apk_commit(self):
        hook = self.root / "lib/apk/commit_hooks.d/00-fixture-change-world"
        hook.write_text("#!/bin/sh\n[ \"$1\" != pre-commit ] || printf 'external-lib\\n' >> /etc/apk/world\n")
        hook.chmod(0o755)
        plan = self.prepare()
        database = (self.root / "lib/apk/db/installed").read_bytes()
        result = self.apply(plan)
        self.assertNotEqual(result.returncode, 0, self.output)
        self.assertEqual((self.root / "lib/apk/db/installed").read_bytes(), database, self.output)
        self.assertEqual((self.root / "usr/share/external-app/version").read_text(), "1.0-r0\n")

    def test_killed_downloader_releases_lock_and_can_resume(self):
        self.server.block_fragment = "external-lib-2.0-r0"
        process = subprocess.Popen(self.command("prepare", "--machine"), stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True, start_new_session=True)
        try:
            self.assertTrue(self.server.entered.wait(15), "updater never reached fixture download")
            os.killpg(process.pid, signal.SIGKILL)
            process.communicate(timeout=10)
            self.server.block_fragment = None
            self.server.release.set()
            result = self.update("resume", "--machine")
            self.assertEqual(result.returncode, 0, self.output)
            tokens = PLAN_TOKEN.findall(result.stdout)
            self.assertEqual(len(tokens), 1, self.output)
            result = self.apply(tokens[0])
            self.assertEqual(result.returncode, 0, self.output)
        finally:
            self.server.release.set()
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.communicate(timeout=10)

    def test_linux_host_root_mutations_are_refused_even_in_owned_chroot(self):
        result = subprocess.run(["unshare", "-Urmp", "--fork", "chroot", self.root,
                                 "/usr/sbin/reliefos-update", "prepare", "--machine"],
                                capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((self.root / "var/lib/reliefos/update").exists(), result.stdout + result.stderr)

    def test_concurrent_prepare_cannot_replace_active_transaction(self):
        self.server.block_fragment = "external-lib-2.0-r0"
        process = subprocess.Popen(self.command("prepare", "--machine"), stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True, start_new_session=True)
        try:
            self.assertTrue(self.server.entered.wait(15), "first updater never reached fixture download")
            result = self.update("prepare", "--machine", timeout=15)
            self.assertNotEqual(result.returncode, 0, self.output)
            self.assertFalse(PLAN_TOKEN.search(result.stdout), self.output)
            self.server.release.set()
            stdout, stderr = process.communicate(timeout=45)
            self.assertEqual(process.returncode, 0, stdout + stderr)
            self.assertEqual(len(PLAN_TOKEN.findall(stdout)), 1, stdout + stderr)
        finally:
            self.server.release.set()
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.communicate(timeout=10)


if __name__ == "__main__":
    unittest.main()
