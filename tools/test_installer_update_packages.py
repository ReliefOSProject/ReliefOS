#!/usr/bin/env python3
"""Exercise offline installer preflight with signed APKs and a read-only target."""
import hashlib
import functools
import http.server
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = Path(os.environ.get("RELIEFOS_TEST_OUTPUT", ROOT / "out/x86_64/release"))
APK = OUTPUT / "upstream/apk/apk.static"
BUSYBOX = OUTPUT / "userland/initramfs/busybox.elf"
UPDATER = ROOT / "userland/storage/reliefos-apk-update"


class UpdatePreflight(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="installer-update-packages-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.work = Path(cls.temporary.name)
        cls.key = cls.work / "test.rsa"
        subprocess.run(["openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt",
                        "rsa_keygen_bits:2048", "-out", cls.key],
                       check=True, capture_output=True)
        cls.key.chmod(0o600)
        cls.base = cls.work / "base"
        for name in ("bin", "usr/sbin", "usr/lib/reliefos", "usr/libexec/reliefos", "sbin", "tmp", "dev", "oldrepo", "newrepo",
                     "target/etc/apk/keys", "target/lib/apk/db", "target/etc/reliefos",
                     "target/home/test", "target/usr/share/external", "target/bin",
                     "target/usr/lib/reliefos", "target/var/lib/reliefos"):
            (cls.base / name).mkdir(parents=True, exist_ok=True)
        shutil.copy2(BUSYBOX, cls.base / "bin/busybox")
        for name in subprocess.check_output([BUSYBOX, "--list"], text=True).splitlines():
            if name != "busybox":
                (cls.base / "bin" / name).symlink_to("busybox")
        shutil.copy2(APK, cls.base / "usr/sbin/apk")
        shutil.copy2(OUTPUT / "userland/initramfs/kernel-bundle.elf",
                     cls.base / "usr/libexec/reliefos/kernel-bundle")
        shutil.copy2(BUSYBOX, cls.base / "target/bin/busybox")
        (cls.base / "target/bin/sh").symlink_to("busybox")
        shutil.copy2(OUTPUT / "rootfs/managed/bin/bash", cls.base / "target/bin/bash")
        (cls.base / "target/var/lib/reliefos/bash-default-v1").write_text("bash\n")
        shutil.copy2(ROOT / "system/rootfs/usr/lib/reliefos/reliefos-migrate",
                     cls.base / "usr/lib/reliefos/reliefos-migrate")
        shutil.copy2(ROOT / "system/rootfs/usr/lib/reliefos/migrate-shell",
                     cls.base / "target/usr/lib/reliefos/migrate-shell")
        subprocess.run(["openssl", "pkey", "-in", cls.key, "-pubout", "-out",
                        cls.base / "target/etc/apk/keys/test.rsa.pub"],
                       check=True, capture_output=True)
        (cls.base / "target/etc/apk/world").write_text("")
        (cls.base / "target/etc/apk/arch").write_text("x86_64\n")
        cls.package("external-fixture", "1.0-r0", "oldrepo")
        cls.package("reliefos-fixture", "1.0-r0", "oldrepo")
        cls.package("reliefos-fixture", "2.0-r0", "newrepo")
        for name in ("oldrepo", "newrepo"):
            cls.index(cls.base / name)
        configuration = cls.work / "old.repositories"
        configuration.write_text("ndx " + str(cls.base / "oldrepo/packages.adb") + "\n")
        cls.apk("--root", cls.base / "target", "--repositories-file", configuration,
                "--no-network", "--no-scripts", "--initdb", "add",
                "reliefos-fixture", "external-fixture")
        (cls.base / "target/etc/reliefos/local.conf").write_text("preserve-local=true\n")
        (cls.base / "target/home/test/document.txt").write_text("用户的文件\n")

    @classmethod
    def apk(cls, *arguments):
        return subprocess.run(["unshare", "-Ur", APK, *map(str, arguments)],
                              check=True, capture_output=True, text=True)

    @classmethod
    def package(cls, name, version, repository):
        files = cls.work / (name + "-" + version)
        (files / "usr/share" / name).mkdir(parents=True)
        (files / "usr/share" / name / "version").write_text(version + "\n")
        cls.apk("mkpkg", "--files", files, "--output",
                cls.base / repository / (name + "-" + version + ".apk"),
                "--info", "name:" + name, "--info", "version:" + version,
                "--info", "arch:x86_64", "--sign-key", cls.key)

    @classmethod
    def index(cls, repository, signed=True):
        args = ["mkndx", "--keys-dir", cls.base / "target/etc/apk/keys",
                "--output", repository / "packages.adb"]
        if signed:
            args.extend(["--sign-key", cls.key])
        else:
            args.insert(0, "--allow-untrusted")
        cls.apk(*args, *sorted(repository.glob("*.apk")))

    def setUp(self):
        self.case = tempfile.TemporaryDirectory(prefix="case-", dir=self.work)
        self.addCleanup(self.case.cleanup)
        self.runner = Path(self.case.name) / "runner"
        shutil.copytree(self.base, self.runner, symlinks=True)
        shutil.copy2(UPDATER, self.runner / "sbin/update")

    def target_snapshot(self):
        target = self.runner / "target"
        return {str(path.relative_to(target)): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in target.rglob("*") if path.is_file() and not path.is_symlink()}

    def transaction(self, mode="--check", expected=None):
        readonly = ('mount --bind "$1/target" "$1/target"; '
                    'mount -o remount,bind,ro "$1/target"; ') if mode == "--check" else ""
        result = subprocess.run([
            "unshare", "-Urmp", "--fork", "sh", "-eu", "-c",
            'mount --make-rprivate /; mount --bind "$1" "$1"; '
            + readonly +
            'cd "$1"; mkdir -p .oldroot; pivot_root . .oldroot; '
            'shift; exec /bin/sh /sbin/update "$@"',
            "sh", self.runner, mode, "/target", "/newrepo", *([expected] if expected else [])],
            text=True, capture_output=True, timeout=60)
        return result

    def check(self):
        before = self.target_snapshot()
        result = self.transaction()
        self.assertEqual(self.target_snapshot(), before, result.stdout + result.stderr)
        return result

    def test_signed_preflight_succeeds_with_readonly_target(self):
        result = self.check()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("reliefos-fixture", result.stdout)

    def test_unsigned_index_is_rejected_before_target_changes(self):
        self.index(self.runner / "newrepo", signed=False)
        result = self.check()
        self.assertNotEqual(result.returncode, 0)
        self.assertRegex(result.stdout + result.stderr, "(?i)signature|trust")

    def test_corrupt_archive_is_rejected_before_target_changes(self):
        package = self.runner / "newrepo/reliefos-fixture-2.0-r0.apk"
        data = bytearray(package.read_bytes())
        data[len(data) // 2] ^= 1
        package.write_bytes(data)
        result = self.check()
        self.assertNotEqual(result.returncode, 0)

    def test_missing_archive_is_rejected_before_target_changes(self):
        (self.runner / "newrepo/reliefos-fixture-2.0-r0.apk").unlink()
        result = self.check()
        self.assertNotEqual(result.returncode, 0)

    def test_apply_and_retry_preserve_user_files_world_and_repository_access(self):
        target = self.runner / "target"
        protected = {name: (target / name).read_bytes() for name in (
            "etc/reliefos/local.conf", "home/test/document.txt",
            "usr/share/external-fixture/version", "etc/apk/world")}
        world_mode = (target / "etc/apk/world").stat().st_mode & 0o777
        for _ in range(2):
            result = self.transaction("--apply")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for name, data in protected.items():
                self.assertEqual((target / name).read_bytes(), data, name)
            self.assertEqual((target / "etc/apk/world").stat().st_mode & 0o777, world_mode)
            repository = target / "usr/share/reliefos/apk/repository"
            self.assertEqual(repository.stat().st_mode & 0o777, 0o755)
            self.assertEqual((repository / "packages.adb").stat().st_mode & 0o777, 0o644)
            self.assertEqual((target / "usr/share/reliefos-fixture/version").read_text(), "2.0-r0\n")
            self.assertFalse((target / "run/reliefos-kernel-activation.defer").exists())

    def test_changed_index_is_rejected_before_apply(self):
        before = self.target_snapshot()
        result = self.transaction("--apply", "0" * 64)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.target_snapshot(), before)

    def test_retry_repairs_an_incomplete_system_package(self):
        result = self.transaction("--apply")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        target = self.runner / "target"
        (target / "usr/share/reliefos-fixture/version").unlink()
        database = target / "lib/apk/db/installed"
        records = database.read_text().split("\n\n")
        for index, record in enumerate(records):
            if "P:reliefos-fixture\n" in record:
                lines = record.splitlines()
                flags = next((i for i, line in enumerate(lines) if line.startswith("f:")), None)
                if flags is None:
                    lines.insert(2, "f:f")
                else:
                    lines[flags] += "f"
                records[index] = "\n".join(lines)
        database.write_text("\n\n".join(records))
        result = self.transaction("--apply")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((target / "usr/share/reliefos-fixture/version").read_text(), "2.0-r0\n")

    def test_tagged_external_packages_use_installed_cached_indexes_offline(self):
        target = self.runner / "target"
        cache = target / "var/cache/apk"
        cache.mkdir(parents=True, exist_ok=True)
        handler = functools.partial(http.server.SimpleHTTPRequestHandler,
                                    directory=str(self.runner / "oldrepo"))
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        url = f"http://127.0.0.1:{server.server_port}/packages.adb"
        configuration = Path(self.case.name) / "repositories"
        configuration.write_text("ndx @addon " + url + "\n")
        try:
            self.apk("--root", target, "--repositories-file", configuration,
                     "--cache-dir", cache, "update")
        finally:
            server.shutdown()
            server.server_close()
            thread.join()
        (target / "etc/apk/repositories").write_text(configuration.read_text())
        world = target / "etc/apk/world"
        world.write_text(world.read_text().replace("external-fixture\n", "external-fixture@addon\n"))
        result = self.check()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
