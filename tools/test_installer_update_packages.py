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
        cls.package("external-library", "1.0-r0", "oldrepo")
        cls.package("external-fixture", "1.0-r0", "oldrepo", ["external-library>=1.0"])
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
    def package(cls, name, version, repository, dependencies=(), payload=None, symlinks=None,
                replaces=(), modes=None):
        files = cls.work / (name + "-" + version)
        if files.exists():
            shutil.rmtree(files)
        (files / "usr/share" / name).mkdir(parents=True, exist_ok=True)
        (files / "usr/share" / name / "version").write_text(version + "\n")
        for relative, text in (payload or {}).items():
            path = files / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        for relative, destination in (symlinks or {}).items():
            path = files / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.symlink_to(destination)
        for relative, mode in (modes or {}).items():
            (files / relative).chmod(mode)
        arguments = ["mkpkg", "--files", files, "--output",
                     cls.base / repository / (name + "-" + version + ".apk"),
                     "--info", "name:" + name, "--info", "version:" + version,
                     "--info", "arch:x86_64", "--sign-key", cls.key]
        if dependencies:
            arguments.extend(["--info", "depends:" + " ".join(dependencies)])
        if replaces:
            arguments.extend(["--info", "replaces:" + " ".join(replaces)])
        cls.apk(*arguments)

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

    def transaction(self, mode="--check", expected=None, cache_readonly=False):
        readonly = ('mount --bind "$1/target" "$1/target"; '
                    'mount -o remount,bind,ro "$1/target"; ') if mode == "--check" else ""
        if cache_readonly:
            readonly += ('mount --bind "$1/target/usr/share/reliefos/apk/repository" '
                         '"$1/target/usr/share/reliefos/apk/repository"; '
                         'mount -o remount,bind,ro "$1/target/usr/share/reliefos/apk/repository"; ')
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

    def test_filesystem_and_bsd_split_transfers_owners_and_preserves_configuration(self):
        old, new = self.runner / "oldrepo", self.runner / "newrepo"
        target = self.runner / "target"
        self.package("reliefos-auth-support-libraries", "4.0-r0", old, payload={
            "lib/libbsd.so.0": "old BSD library\n", "lib/libmd.so.0": "local MD library\n"})
        self.package("reliefos-storage-filesystems", "4.0-r0", old, payload={
            "usr/sbin/mkfs.ext4": "old formatter\n", "usr/sbin/mkfs.exfat": "local exFAT\n"},
            symlinks={"sbin/mkfs.ext4": "../usr/sbin/mkfs.ext4"})
        # Older rootfs fallback ownership also put configuration and extra
        # tools in base; all old owners must join the same normal upgrade.
        self.package("reliefos-base", "4.0-r0", old, payload={
            "usr/lib/libext2fs.so.2": "old ext library\n", "etc/mke2fs.conf": "old defaults\n"})
        self.package("reliefos-base", "5.0-r0", new)
        self.package("libbsd", "0.12.2-r0", new, payload={"usr/lib/libbsd.so.0": "official BSD library\n"})
        self.package("e2fsprogs", "1.47.4-r0", new, payload={
            "sbin/mkfs.ext4": "official formatter\n", "usr/lib/libext2fs.so.2": "official ext library\n",
            "etc/mke2fs.conf": "new defaults\n"})
        self.package("reliefos-auth-support-libraries", "5.0-r0", new, ["libbsd"],
                     payload={"lib/libmd.so.0": "local MD library\n"})
        self.package("reliefos-storage-filesystems", "5.0-r0", new, ["e2fsprogs"],
                     payload={"usr/sbin/mkfs.exfat": "local exFAT\n"},
                     symlinks={"usr/sbin/mkfs.ext4": "../../sbin/mkfs.ext4"})
        self.index(old)
        self.index(new)
        repositories = self.runner / "old.repositories"
        repositories.write_text("ndx " + str(old / "packages.adb") + "\n")
        self.apk("--root", target, "--repositories-file", repositories,
                 "--no-network", "--no-scripts", "add", "reliefos-auth-support-libraries",
                 "reliefos-storage-filesystems", "reliefos-base")
        (target / "etc/mke2fs.conf").write_text("administrator defaults\n")
        checked = self.check()
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
        for _ in range(2):
            result = self.transaction("--apply")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertFalse((target / "lib/libbsd.so.0").exists())
            self.assertEqual((target / "sbin/mkfs.ext4").read_text(), "official formatter\n")
            self.assertFalse((target / "sbin/mkfs.ext4").is_symlink())
            self.assertEqual(os.readlink(target / "usr/sbin/mkfs.ext4"), "../../sbin/mkfs.ext4")
            self.assertEqual((target / "etc/mke2fs.conf").read_text(), "administrator defaults\n")
            self.assertEqual((target / "lib/libmd.so.0").read_text(), "local MD library\n")
            self.assertEqual((target / "usr/sbin/mkfs.exfat").read_text(), "local exFAT\n")
            for path, owner in (("sbin/mkfs.ext4", "e2fsprogs"),
                                ("usr/lib/libext2fs.so.2", "e2fsprogs"),
                                ("usr/lib/libbsd.so.0", "libbsd")):
                output = self.apk("--root", target, "info", "--who-owns", "/" + path).stdout
                self.assertIn("owned by " + owner + "-", output)

    def test_util_linux_transfer_retains_shadow_pam_and_official_su_modes(self):
        old, new = self.runner / "oldrepo", self.runner / "newrepo"
        target = self.runner / "target"
        shadow = {"bin/login": "shadow login\n", "usr/bin/newgrp": "shadow newgrp\n",
                  "sbin/nologin": "shadow nologin\n"}
        self.package("reliefos-base", "5.0-r0", old, payload=shadow)
        self.package("reliefos-authentication", "5.0-r0", old, payload={
            "bin/su": "local su\n", "etc/pam.d/su-l": "product policy\n"})
        self.package("reliefos-storage-util-linux", "5.0-r0", old, payload={
            "usr/sbin/fdisk": "local fdisk\n", "usr/lib/libblkid.so.1": "local blkid\n"},
            symlinks={"sbin/fdisk": "../usr/sbin/fdisk"})
        self.package("reliefos-base", "6.0-r0", new)
        self.package("util-linux-login", "2.42.4-r3", new, payload={
            **{name: "official overlapping tool\n" for name in shadow},
            "bin/su": "official su\n", "etc/pam.d/su-l": "upstream policy\n"},
            modes={"bin/su": 0o4755})
        self.package("util-linux-misc", "2.42.4-r3", new,
                     payload={"sbin/fdisk": "official fdisk\n"})
        self.package("libblkid", "2.42.4-r3", new,
                     payload={"usr/lib/libblkid.so.1": "official blkid\n"})
        self.package("reliefos-authentication", "6.0-r0", new, ["util-linux-login"],
                     payload={**shadow, "etc/pam.d/su-l": "product policy\n"},
                     replaces=["util-linux-login"])
        self.package("reliefos-storage-util-linux", "6.0-r0", new,
                     ["util-linux-misc", "libblkid"],
                     symlinks={"usr/sbin/fdisk": "../../sbin/fdisk"})
        self.index(old)
        self.index(new)
        repositories = self.runner / "old.repositories"
        repositories.write_text("ndx " + str(old / "packages.adb") + "\n")
        self.apk("--root", target, "--repositories-file", repositories,
                 "--no-network", "--no-scripts", "add", "reliefos-base",
                 "reliefos-authentication", "reliefos-storage-util-linux")
        (target / "etc/pam.d/su-l").write_text("administrator policy\n")
        checked = self.check()
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
        for _ in range(2):
            result = self.transaction("--apply")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for name, text in shadow.items():
                self.assertEqual((target / name).read_text(), text)
                owner = self.apk("--root", target, "info", "--who-owns", "/" + name).stdout
                self.assertIn("owned by reliefos-authentication-", owner)
            self.assertEqual((target / "etc/pam.d/su-l").read_text(), "administrator policy\n")
            self.assertEqual((target / "bin/su").stat().st_mode & 0o7777, 0o4755)
            self.assertEqual((target / "bin/su").read_text(), "official su\n")
            self.assertEqual(os.readlink(target / "usr/sbin/fdisk"), "../../sbin/fdisk")
            self.assertFalse((target / "sbin/fdisk").is_symlink())
            for path, package in (("bin/su", "util-linux-login"),
                                  ("sbin/fdisk", "util-linux-misc"),
                                  ("usr/lib/libblkid.so.1", "libblkid")):
                owner = self.apk("--root", target, "info", "--who-owns", "/" + path).stdout
                self.assertIn("owned by " + package + "-", owner)
        # A later upstream-only upgrade must still honor the already installed
        # local replacement, without reinstalling/changing authentication.
        self.package("util-linux-login", "2.42.5-r0", new, payload={
            **{name: "new upstream overlap\n" for name in shadow},
            "bin/su": "upgraded official su\n", "etc/pam.d/su-l": "new upstream policy\n"},
            modes={"bin/su": 0o4755})
        self.index(new)
        repositories.write_text("ndx " + str(new / "packages.adb") + "\n")
        self.apk("--root", target, "--repositories-file", repositories,
                 "--no-network", "--no-scripts", "upgrade", "util-linux-login")
        for name, text in shadow.items():
            self.assertEqual((target / name).read_text(), text)
        self.assertEqual((target / "bin/su").read_text(), "upgraded official su\n")
        self.assertEqual((target / "bin/su").stat().st_mode & 0o7777, 0o4755)
        self.assertEqual((target / "etc/pam.d/su-l").read_text(), "administrator policy\n")

    def test_libmd_transfer_removes_old_development_files_and_keeps_legacy_links(self):
        old, new = self.runner / "oldrepo", self.runner / "newrepo"
        target = self.runner / "target"
        self.package("reliefos-auth-support-libraries", "6.0-r0", old, payload={
            "lib/libmd.so.0.3.0": "old local library\n", "lib/libmd.a": "old archive\n",
            "usr/share/licenses/libmd/COPYING": "old license\n"},
            symlinks={"lib/libmd.so": "libmd.so.0.3.0", "lib/libmd.so.0": "libmd.so.0.3.0"})
        self.package("reliefos-base", "6.0-r0", old, payload={
            "usr/include/md5.h": "old header\n", "lib/pkgconfig/libmd.pc": "old build flags\n"})
        self.package("libmd", "1.3.0-r0", new,
                     payload={"usr/lib/libmd.so.0.3.0": "official library\n"},
                     symlinks={"usr/lib/libmd.so.0": "libmd.so.0.3.0"})
        self.package("libmd-doc", "1.3.0-r0", new,
                     payload={"usr/share/licenses/libmd/COPYING": "official license\n"})
        self.package("reliefos-auth-support-libraries", "7.0-r0", new, ["libmd", "libmd-doc"],
                     symlinks={"lib/libmd.so.0": "../usr/lib/libmd.so.0",
                               "lib/libmd.so": "libmd.so.0", "lib/libmd.so.0.3.0": "libmd.so.0"})
        self.package("reliefos-base", "7.0-r0", new)
        self.index(old)
        self.index(new)
        repositories = self.runner / "old.repositories"
        repositories.write_text("ndx " + str(old / "packages.adb") + "\n")
        self.apk("--root", target, "--repositories-file", repositories, "--no-network",
                 "--no-scripts", "add", "reliefos-base", "reliefos-auth-support-libraries")
        protected = {name: (target / name).read_bytes() for name in (
            "etc/apk/world", "etc/reliefos/local.conf", "home/test/document.txt")}
        checked = self.check()
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
        for _ in range(2):
            result = self.transaction("--apply")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for name, content in protected.items():
                self.assertEqual((target / name).read_bytes(), content, name)
            for path in ("lib/libmd.a", "usr/include/md5.h", "lib/pkgconfig/libmd.pc"):
                self.assertFalse((target / path).exists(), path)
            self.assertEqual(os.readlink(target / "lib/libmd.so.0"), "../usr/lib/libmd.so.0")
            for name in ("libmd.so", "libmd.so.0.3.0"):
                self.assertEqual(os.readlink(target / "lib" / name), "libmd.so.0")
                self.assertEqual((target / "lib" / name).read_text(), "official library\n")
            for path, package in (("usr/lib/libmd.so.0", "libmd"),
                                  ("usr/share/licenses/libmd/COPYING", "libmd-doc"),
                                  ("lib/libmd.so.0", "reliefos-auth-support-libraries")):
                owner = self.apk("--root", target, "info", "--who-owns", "/" + path).stdout
                self.assertIn("owned by " + package + "-", owner)

    def test_old_console_ca_placeholder_is_replaced_by_real_bundle(self):
        old, new = self.runner / "oldrepo", self.runner / "newrepo"
        target = self.runner / "target"
        self.package("ca-certificates-bundle", "4.0-r0", old)
        self.package("reliefos-trust", "4.0-r0", old, ["ca-certificates-bundle"])
        self.package("ca-certificates-bundle", "20261020-r0", new, payload={
            "etc/ssl/certs/ca-certificates.crt": "official bundle fixture\n"})
        self.package("reliefos-trust", "5.0-r0", new,
                     ["ca-certificates-bundle>=20261020-r0"])
        self.index(old)
        self.index(new)
        repositories = self.runner / "old.repositories"
        repositories.write_text("ndx " + str(old / "packages.adb") + "\n")
        self.apk("--root", target, "--repositories-file", repositories,
                 "--no-network", "--no-scripts", "add", "reliefos-trust")
        self.assertFalse((target / "etc/ssl/certs/ca-certificates.crt").exists())
        checked = self.check()
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
        for _ in range(2):
            result = self.transaction("--apply")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual((target / "etc/ssl/certs/ca-certificates.crt").read_text(),
                             "official bundle fixture\n")
            owner = self.apk("--root", target, "info", "--who-owns",
                             "/etc/ssl/certs/ca-certificates.crt").stdout
            self.assertIn("owned by ca-certificates-bundle-20261020-r0", owner)

    def cache_events(self, result):
        events = []
        for line in result.stdout.splitlines():
            fields = line.split("\t")
            if fields[:2] != ["activity", "cache"]:
                continue
            self.assertEqual(len(fields), 10, line)
            events.append({"action": fields[2], "package": fields[3],
                           **dict(zip(("done", "total", "bytes", "size", "reused", "status"),
                                      map(int, fields[4:])))})
        return events

    def test_identical_offline_cache_is_reused_without_replacing_files(self):
        first = self.transaction("--apply")
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        cache = self.runner / "target/usr/share/reliefos/apk/repository"
        before = {p.name: (p.stat().st_ino, p.stat().st_mtime_ns, p.read_bytes()) for p in cache.iterdir()}
        again = self.transaction("--apply")
        self.assertEqual(again.returncode, 0, again.stdout + again.stderr)
        self.assertEqual(before, {p.name: (p.stat().st_ino, p.stat().st_mtime_ns, p.read_bytes()) for p in cache.iterdir()})
        events = self.cache_events(again)
        final = events[-1]
        self.assertEqual(final["done"], len(before))
        self.assertEqual(final["total"], len(before))
        self.assertEqual(final["bytes"], sum(len(value[2]) for value in before.values()))
        self.assertEqual(final["bytes"], final["size"])
        self.assertEqual(final["reused"], final["size"])
        self.assertNotIn("copying", [event["action"] for event in events])
        self.assertEqual([event["bytes"] for event in events], sorted(event["bytes"] for event in events))

    def test_different_cache_bytes_are_replaced_even_with_matching_size(self):
        first = self.transaction("--apply")
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        cache = self.runner / "target/usr/share/reliefos/apk/repository"
        archive = cache / "reliefos-fixture-2.0-r0.apk"
        expected = archive.read_bytes()
        archive.write_bytes(bytes([expected[0] ^ 1]) + expected[1:])
        result = self.transaction("--apply")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(archive.read_bytes(), expected)
        self.assertTrue(any(event["action"] == "copying" and event["package"] == archive.name
                            for event in self.cache_events(result)))
        self.assertFalse(list(cache.glob(".update-cache.*")))

    def test_cache_failure_identifies_the_file_and_preserves_old_bytes(self):
        first = self.transaction("--apply")
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        cache = self.runner / "target/usr/share/reliefos/apk/repository"
        archive = cache / "reliefos-fixture-2.0-r0.apk"
        archive.write_bytes(b"old archive must survive a failed publication")
        result = self.transaction("--apply", cache_readonly=True)
        self.assertNotEqual(result.returncode, 0)
        failure = self.cache_events(result)[-1]
        self.assertEqual(failure["action"], "failed")
        self.assertEqual(failure["package"], archive.name)
        self.assertGreater(failure["status"], 0)
        self.assertIn("failed step=cache operation=copying", result.stderr)
        self.assertEqual(archive.read_bytes(), b"old archive must survive a failed publication")
        self.assertFalse(list(cache.glob(".update-cache.*")))
        self.assertFalse((self.runner / "target/run/reliefos-kernel-activation.defer").exists())

    def test_cache_links_are_refused_and_old_temporary_links_are_not_followed(self):
        cache = self.runner / "target/usr/share/reliefos/apk/repository"
        cache.mkdir(parents=True)
        outside = self.runner / "outside"
        outside.write_bytes(b"unrelated data")
        name = "reliefos-fixture-2.0-r0.apk"
        (cache / (name + ".new")).symlink_to("/outside")
        result = self.transaction("--apply")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(outside.read_bytes(), b"unrelated data")
        (cache / name).unlink()
        (cache / name).symlink_to("/outside")
        result = self.transaction("--apply")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(outside.read_bytes(), b"unrelated data")
        self.assertEqual(self.cache_events(result)[-1]["action"], "failed")

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

    def select_external_tag(self, selection="external-fixture@testing"):
        """Record a real APK selection, including its installed repository tag."""
        configuration = Path(self.case.name) / "selection.repositories"
        configuration.write_text("ndx @testing " + str(self.runner / "oldrepo/packages.adb") + "\n")
        self.apk("--root", self.runner / "target", "--repositories-file", configuration,
                 "--no-network", "--no-scripts", "add", selection)

    def assert_tagged_update(self):
        target = self.runner / "target"
        def external_records():
            records = {}
            for record in (target / "lib/apk/db/installed").read_text().split("\n\n"):
                fields = {line[:1]: line[2:] for line in record.splitlines()
                          if line.startswith(("P:", "V:", "C:", "s:", "f:", "D:"))}
                if fields.get("P", "").startswith("external-"):
                    records[fields["P"]] = fields
            return records
        metadata = external_records()
        protected = {name: (target / name).read_bytes() for name in (
            "etc/apk/world", "etc/apk/repositories", "etc/reliefos/local.conf",
            "usr/share/external-fixture/version", "usr/share/external-library/version", "home/test/document.txt")}
        result = self.check()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("opening from cache", result.stdout + result.stderr)
        result = self.transaction("--apply")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for name, contents in protected.items():
            self.assertEqual((target / name).read_bytes(), contents, name)
        self.assertEqual(external_records(), metadata)
        self.assertEqual((target / "usr/share/reliefos-fixture/version").read_text(), "2.0-r0\n")

    def test_tagged_external_packages_without_index_cache(self):
        self.select_external_tag()
        target = self.runner / "target"
        (target / "etc/apk/repositories").write_text(
            "@testing https://mirrors.tuna.tsinghua.edu.cn/alpine/edge/testing\n")
        for cache in (target / "var/cache/apk", target / "etc/apk/cache"):
            if cache.is_symlink():
                cache.unlink()
            elif cache.exists():
                shutil.rmtree(cache)
        self.assert_tagged_update()

    def test_retry_after_committed_apk_failure_preserves_tagged_world(self):
        self.select_external_tag()
        target = self.runner / "target"
        (target / "etc/apk/repositories").write_text("@testing https://offline.invalid/testing\n")
        world_before = (target / "etc/apk/world").read_bytes()
        apk = self.runner / "usr/sbin/apk"
        apk.rename(apk.with_name("apk.real"))
        apk.write_text('''#!/bin/sh
/usr/sbin/apk.real "$@"
result=$?
[ "$result" -eq 0 ] || exit "$result"
if [ "${1:-}" = --root ] && [ "${2:-}" = /target ]; then
    case " $* " in
        *' upgrade '*)
            if [ ! -e /tmp/failed-after-commit ]; then
                touch /tmp/failed-after-commit
                echo 'ERROR: injected failure after APK committed' >&2
                exit 42
            fi ;;
    esac
fi
exit 0
''')
        apk.chmod(0o755)
        failed = self.transaction("--apply")
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn("injected failure after APK committed", failed.stdout + failed.stderr)
        self.assertEqual((target / "usr/share/reliefos-fixture/version").read_text(), "2.0-r0\n")
        self.assertEqual((target / "etc/apk/world").read_bytes(), world_before)
        self.assertFalse((target / "run/reliefos-kernel-activation.defer").exists())
        self.assert_tagged_update()

    def test_tagged_external_packages_with_deleted_repository_configuration(self):
        self.select_external_tag()
        (self.runner / "target/etc/apk/repositories").write_text("# removed remote repository\n")
        self.assert_tagged_update()

    def test_multiple_tags_and_pinned_external_version(self):
        self.select_external_tag("external-fixture@testing=1.0-r0")
        target = self.runner / "target"
        world = target / "etc/apk/world"
        world.write_text(world.read_text().replace("reliefos-fixture\n", "reliefos-fixture@alpine\n"))
        (target / "etc/apk/repositories").write_text(
            "@testing https://offline.invalid/testing\n@alpine https://offline.invalid/alpine\n")
        self.assert_tagged_update()

    def test_corrupt_external_cache_is_not_used_by_offline_update(self):
        self.select_external_tag()
        target = self.runner / "target"
        cache = target / "var/cache/apk"
        cache.mkdir(parents=True, exist_ok=True)
        (cache / "APKINDEX.deadbeef.tar.gz").write_bytes(b"corrupt external index")
        (target / "etc/apk/cache").symlink_to("/var/cache/apk")
        (target / "etc/apk/repositories").write_text("@testing https://offline.invalid/testing\n")
        self.assert_tagged_update()

    def test_missing_tagged_package_is_not_silently_ignored(self):
        self.select_external_tag()
        world = self.runner / "target/etc/apk/world"
        world.write_text(world.read_text() + "missing-extra@testing\n")
        result = self.check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing-extra", result.stdout + result.stderr)

    def test_unsatisfied_tagged_version_is_rejected_before_apply(self):
        self.select_external_tag()
        target = self.runner / "target"
        world = target / "etc/apk/world"
        world.write_text(world.read_text().replace("external-fixture@testing", "external-fixture@testing>=9.0-r0"))
        before = self.target_snapshot()
        for mode in ("--check", "--apply"):
            with self.subTest(mode=mode):
                result = self.transaction(mode)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("external-fixture", result.stdout + result.stderr)
                self.assertEqual(self.target_snapshot(), before)

    def test_unavailable_new_dependency_is_rejected_before_apply(self):
        self.package("reliefos-fixture", "2.0-r0", self.runner / "newrepo", ["missing-library>=2.0"])
        self.index(self.runner / "newrepo")
        before = self.target_snapshot()
        result = self.transaction("--apply")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing-library", result.stdout + result.stderr)
        self.assertEqual(self.target_snapshot(), before)

    def test_media_external_upgrade_is_not_selected_unnecessarily(self):
        self.select_external_tag()
        (self.runner / "target/etc/apk/repositories").write_text("@testing https://offline.invalid/testing\n")
        self.package("external-fixture", "3.0-r0", self.runner / "newrepo", ["external-library>=1.0"])
        self.index(self.runner / "newrepo")
        self.assert_tagged_update()

    def test_media_dependency_can_be_installed_without_external_index(self):
        self.select_external_tag()
        target = self.runner / "target"
        (target / "etc/apk/repositories").write_text("@testing https://offline.invalid/testing\n")
        self.package("media-library", "1.0-r0", self.runner / "newrepo")
        self.package("reliefos-fixture", "2.0-r0", self.runner / "newrepo", ["media-library>=1.0"])
        self.index(self.runner / "newrepo")
        self.assert_tagged_update()
        self.assertEqual((target / "usr/share/media-library/version").read_text(), "1.0-r0\n")

    def test_external_package_conflict_is_rejected_before_apply(self):
        self.select_external_tag()
        self.package("reliefos-fixture", "2.0-r0", self.runner / "newrepo", ["!external-fixture"])
        self.index(self.runner / "newrepo")
        before = self.target_snapshot()
        result = self.transaction("--apply")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("external-fixture", result.stdout + result.stderr)
        self.assertEqual(self.target_snapshot(), before)

    def test_tagged_external_packages_with_cached_indexes_and_repository_snippets(self):
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
        configuration.write_text("ndx @testing " + url + "\n")
        try:
            self.apk("--root", target, "--repositories-file", configuration,
                     "--cache-dir", cache, "update")
        finally:
            server.shutdown()
            server.server_close()
            thread.join()
        world = target / "etc/apk/world"
        world.write_text(world.read_text().replace("external-fixture\n", "external-fixture@testing\n"))
        locations = [target / name for name in (
            "etc/apk/repositories", "etc/apk/repositories.d/testing.list",
            "lib/apk/repositories.d/testing.list")]
        for location in locations:
            location.parent.mkdir(parents=True, exist_ok=True)
            for prefix in ("ndx @testing", "@testing"):
                with self.subTest(repository_syntax=prefix, configuration=location.relative_to(target)):
                    location.write_text(prefix + " " + url + "\n")
                    result = self.check()
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            location.unlink()
        # The commit uses the same generated repository file as preflight.
        (target / "etc/apk/repositories").write_text("@testing " + url + "\n")
        preserved_world = world.read_bytes()
        external_version = (target / "usr/share/external-fixture/version").read_bytes()
        result = self.transaction("--apply")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(world.read_bytes(), preserved_world)
        self.assertEqual((target / "usr/share/external-fixture/version").read_bytes(), external_version)
        self.assertEqual((target / "usr/share/reliefos-fixture/version").read_text(), "2.0-r0\n")


if __name__ == "__main__":
    unittest.main()
