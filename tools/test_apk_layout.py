#!/usr/bin/env python3
"""Validate apk preparation without installing packages or inventing a database."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

import reliefos_layout as layout
import leonos_layout as legacy_layout

ROOT = Path(__file__).resolve().parents[1]
SEED = ROOT / "system/rootfs"
APK_DIRECTORIES = (
    "etc/apk", "etc/apk/commit_hooks.d", "etc/apk/keys",
    "etc/apk/protected_paths.d", "etc/apk/repositories.d",
    "lib/apk", "lib/apk/db", "lib/apk/exec", "lib/apk/keys",
    "lib/apk/repositories.d", "lib/apk/commit_hooks.d", "var/cache/apk",
)
CA_LINKS = {
    "etc/apk/ca.pem": "../ssl/certs/ca-certificates.crt",
    "etc/ssl/cert.pem": "certs/ca-certificates.crt",
}
STATE_FILES = (
    "lib/apk/db/installed", "lib/apk/db/installed.adb", "lib/apk/db/lock",
    "lib/apk/db/scripts.tar", "lib/apk/db/scripts.tar.gz",
    "lib/apk/db/triggers", "var/log/apk.log",
)
SEED_FILES = tuple(sorted(p.relative_to(SEED).as_posix() for parent in
                         (SEED / "etc/apk", SEED / "usr/share/licenses/alpine-keys")
                         for p in parent.rglob("*") if p.is_file()))


class ApkSeedTests(unittest.TestCase):
    def test_reliefos_layout_module_is_canonical_and_old_import_forwards(self):
        import reliefos_layout

        self.assertIs(legacy_layout.layout_directories, layout.layout_directories)
        self.assertIs(legacy_layout.apply_root_symlinks, layout.apply_root_symlinks)
        self.assertEqual(legacy_layout.ETC_LEONOS, layout.ETC_RELIEFOS)

    def test_reliefos_layout_constants_match_public_header(self):
        contract = (ROOT / "include/reliefos/layout.h").read_text(encoding="ascii")
        pairs = {
            "ETC_RELIEFOS": "RELIEFOS_LAYOUT_ETC_RELIEFOS",
            "VAR_LIB_RELIEFOS": "RELIEFOS_LAYOUT_VAR_LIB_RELIEFOS",
            "VAR_CACHE_RELIEFOS": "RELIEFOS_LAYOUT_VAR_CACHE_RELIEFOS",
            "RUN_RELIEFOS": "RELIEFOS_LAYOUT_RUN_RELIEFOS",
            "RELIEFOS_LIB": "RELIEFOS_LAYOUT_RELIEFOS_LIB",
            "RELIEFOS_APPS": "RELIEFOS_LAYOUT_RELIEFOS_APPS",
            "RELIEFOS_TESTS": "RELIEFOS_LAYOUT_RELIEFOS_TESTS",
            "RELIEFOS_SHARE": "RELIEFOS_LAYOUT_RELIEFOS_SHARE",
            "RELIEFOS_RESOURCES": "RELIEFOS_LAYOUT_RELIEFOS_RESOURCES",
            "RELIEFOS_FONTS": "RELIEFOS_LAYOUT_RELIEFOS_FONTS",
            "RELIEFOS_DOC": "RELIEFOS_LAYOUT_RELIEFOS_DOC",
            "P_ETC_RELIEFOS": "RELIEFOS_LAYOUT_ETC_RELIEFOS",
            "P_VAR_LIB_RELIEFOS": "RELIEFOS_LAYOUT_VAR_LIB_RELIEFOS",
            "P_VAR_CACHE_RELIEFOS": "RELIEFOS_LAYOUT_VAR_CACHE_RELIEFOS",
            "P_RUN_RELIEFOS": "RELIEFOS_LAYOUT_RUN_RELIEFOS",
            "P_RELIEFOS_LIB": "RELIEFOS_LAYOUT_RELIEFOS_LIB",
            "P_RELIEFOS_APPS": "RELIEFOS_LAYOUT_RELIEFOS_APPS",
            "P_RELIEFOS_TESTS": "RELIEFOS_LAYOUT_RELIEFOS_TESTS",
            "P_RELIEFOS_SHARE": "RELIEFOS_LAYOUT_RELIEFOS_SHARE",
            "P_RELIEFOS_RESOURCES": "RELIEFOS_LAYOUT_RELIEFOS_RESOURCES",
            "P_RELIEFOS_FONTS": "RELIEFOS_LAYOUT_RELIEFOS_FONTS",
            "P_RELIEFOS_DOC": "RELIEFOS_LAYOUT_RELIEFOS_DOC",
        }
        for python_name, c_name in pairs.items():
            match = re.search(rf'^#define {c_name} "([^"]+)"$', contract, re.MULTILINE)
            self.assertIsNotNone(match, c_name)
            expected = match.group(1)
            if not python_name.startswith("P_"):
                expected = expected.lstrip("/")
            self.assertEqual(getattr(layout, python_name), expected, python_name)

    def test_layout_and_shared_ca_links(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-apk-layout-") as directory:
            root = Path(directory)
            layout.layout_directories(root)
            bundle = root / "etc/ssl/certs/ca-certificates.crt"
            shutil.copyfile(ROOT / "system/certs/cacert.pem", bundle)
            layout.apply_root_symlinks(root)
            layout.layout_directories(root)
            layout.apply_root_symlinks(root)
            for name in APK_DIRECTORIES:
                with self.subTest(path=name):
                    self.assertEqual(layout.ROOT_DIRECTORIES[name], 0o755)
                    self.assertFalse((root / name).is_symlink())
                    self.assertEqual((root / name).stat().st_mode & 0o7777, 0o755)
            for name, target in CA_LINKS.items():
                self.assertEqual(os.readlink(root / name), target)
                self.assertEqual((root / name).resolve(), bundle)

    def test_keys_match_official_checksums(self):
        manifest = json.loads((SEED / "usr/share/licenses/alpine-keys/SOURCE.json").read_text())
        keys = SEED / "etc/apk/keys"
        self.assertEqual(set(p.name for p in keys.iterdir()), set(manifest["sha512"]))
        self.assertEqual(len(manifest["sha512"]), 3)
        for name, expected in manifest["sha512"].items():
            with self.subTest(key=name):
                data = (keys / name).read_bytes()
                self.assertNotIn(b"PRIVATE KEY", data)
                self.assertEqual(hashlib.sha512(data).hexdigest(), expected)
                subprocess.run(["openssl", "pkey", "-pubin", "-in", str(keys / name),
                                "-noout"], check=True, capture_output=True)

    def test_repository_policy_and_empty_database(self):
        self.assertEqual((SEED / "etc/apk/arch").read_text(), "x86_64\n")
        lines = (SEED / "etc/apk/repositories").read_text().splitlines()
        self.assertEqual([line for line in lines if line and not line.startswith("#")], [
            "https://mirrors.tuna.tsinghua.edu.cn/alpine/v3.24/main",
            "https://mirrors.tuna.tsinghua.edu.cn/alpine/v3.24/community",
            "@alpine https://mirrors.tuna.tsinghua.edu.cn/alpine/v3.24/main",
            "@alpine https://mirrors.tuna.tsinghua.edu.cn/alpine/v3.24/community",
            "@testing https://mirrors.tuna.tsinghua.edu.cn/alpine/edge/testing",
        ])
        options = (SEED / "etc/apk/config").read_text().splitlines()
        self.assertEqual([line for line in options if line and not line.startswith("#")],
                         ["timeout 60", "wait 30", "cache-dir /var/cache/apk"])
        self.assertFalse((SEED / "etc/apk/world").read_text().strip())
        self.assertIn("ID=reliefos", (SEED / "etc/os-release").read_text())
        self.assertFalse((SEED / "etc/alpine-release").exists())
        for name in STATE_FILES:
            self.assertFalse((SEED / name).exists(), name)
        self.assertEqual(list((SEED / "lib/apk/db").glob("*")), [])

    def test_protected_paths(self):
        lines = (SEED / "etc/apk/protected_paths.d/leonos.list").read_text().splitlines()
        rules = {line for line in lines if line and not line.startswith("#")}
        self.assertTrue({"!etc/passwd", "!etc/shadow", "!etc/pam.d", "!etc/sudoers",
                         "!etc/leonos", "!etc/reliefos", "!etc/os-release", "!etc/fstab"} <= rules)
        self.assertTrue(all(re.fullmatch(r"!etc/[a-zA-Z0-9_/.-]+", rule) for rule in rules))


def check_images():
    def debugfs(image, command):
        return subprocess.check_output(["debugfs", "-R", command, str(image)],
                                       stderr=subprocess.DEVNULL)

    def stat(image, name, kind, mode):
        output = debugfs(image, f"stat /{name}").decode()
        assert f"Type: {kind}" in output, (image, name, output)
        assert re.search(rf"Mode:\s+0?{mode:o}\b", output), (image, name, output)
        assert re.search(r"User:\s+0\s+Group:\s+0\b", output), (image, name, output)
        return output

    for image, prefixes in ((ROOT / "build/live/root.ext2", ("",)),
                            (ROOT / "build/install/root.fat", ("", "install/root/"))):
        assert image.is_file(), f"Build images first: {image}"
        for prefix in prefixes:
            manifest_path = ROOT / ("build/apk/normal/manifest.json" if image.name == "root.ext2"
                                    else "build/install/apk-installed/manifest.json" if prefix
                                    else "build/install/apk-runtime/manifest.json")
            manifest = json.loads(manifest_path.read_text())
            package_files = {entry["path"]: entry for entry in manifest["files"]}
            for name in APK_DIRECTORIES:
                stat(image, prefix + name, "directory", 0o755)
            for name in SEED_FILES:
                if name in {"etc/apk/world", "etc/apk/repositories"}:
                    continue
                stat(image, prefix + name, "regular", 0o644)
                assert debugfs(image, f"cat /{prefix}{name}") == (SEED / name).read_bytes(), name
            for name, target in CA_LINKS.items():
                output = stat(image, prefix + name, "symlink", 0o777)
                assert f'Fast link dest: "{target}"' in output, (image, prefix, name)
            assert debugfs(image, f"cat /{prefix}etc/ssl/certs/ca-certificates.crt") == (
                ROOT / "system/certs/cacert.pem").read_bytes()
            database = debugfs(image, f"cat /{prefix}lib/apk/db/installed")
            world = debugfs(image, f"cat /{prefix}etc/apk/world")
            assert b"P:reliefos-apk-tools\n" in database and b"P:reliefos-fastfetch\n" in database
            assert b"reliefos-fastfetch\n" in world
            assert b"P:reliefos-musl-dev\n" in database and b"reliefos-musl-dev\n" in world
            repositories = debugfs(image, f"cat /{prefix}etc/apk/repositories")
            assert repositories == (b"ndx /usr/share/reliefos/apk/repository/packages.adb\n" +
                                    (SEED / "etc/apk/repositories").read_bytes())
            assert debugfs(image, f"cat /{prefix}usr/include/stdio.h") == (
                ROOT / "build/musl/sysroot/include/stdio.h").read_bytes()
            assert debugfs(image, f"cat /{prefix}usr/lib/crt1.o") == (
                ROOT / "build/musl/sysroot/lib/crt1.o").read_bytes()
            assert hashlib.sha256(debugfs(image, f"cat /{prefix}sbin/apk")).hexdigest() == (
                "5118a57ae7c07e13268a754f78aa9c7d39a0bed708bb11c101d78e2a884cee5d")
            assert debugfs(image, f"cat /{prefix}usr/share/reliefos/apk/repository/packages.adb")
            for name in ("usr/lib/reliefos/apps/login/login.elf", "usr/lib/reliefos/libreliefos.so.2", "usr/lib/leonos/libleonos.so.2",
                         "sbin/apk", "usr/bin/vim", "usr/bin/sudo", "lib/ld-musl-x86_64.so.1"):
                entry = package_files["/" + name]
                assert hashlib.sha256(debugfs(image, f"cat /{prefix}{name}")).hexdigest() == entry["sha256"], (image, prefix, name)
                stat(image, prefix + name, "regular", int(entry["mode"], 8))
            assert debugfs(image, f"cat /{prefix}usr/share/doc/reliefos/APK_PREPARATION.md") == (
                ROOT / "docs/APK_PREPARATION.md").read_bytes()
            print(f"PASS {image.relative_to(ROOT)} {prefix or '/'}: real APK database, executable, repository, modes and trust")


def check_upstream(apk: Path):
    """Run a separately verified upstream apk against a disposable host root."""
    apk = apk.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="reliefos-apk-reference-") as directory:
        root = Path(directory)
        layout.layout_directories(root)
        for name in SEED_FILES:
            target = root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(SEED / name, target)
        shutil.copyfile(ROOT / "system/certs/cacert.pem", root / "etc/ssl/certs/ca-certificates.crt")
        layout.apply_root_symlinks(root)
        # apk config is relative to the caller, unlike database files under --root.
        env = dict(os.environ, APK_CONFIG=str(root / "etc/apk/config"))
        command = [str(apk), "--root", str(root), "--cache-dir", str(root / "var/cache/apk")]
        result = subprocess.run(command + ["--no-network", "--repositories-file", "/dev/null", "info"],
                                env=env, check=True, text=True, capture_output=True, timeout=30)
        assert not result.stdout.strip(), result.stdout
        print("PASS upstream apk reads empty world/database without --initdb")
        result = subprocess.run(command + ["update"], env=env, text=True,
                                capture_output=True, timeout=180)
        print(result.stdout, end="")
        assert result.returncode == 0, result.stderr
        assert "WARNING" not in result.stderr and "ERROR" not in result.stderr, result.stderr
        result = subprocess.run(command + ["--no-network", "search", "--exact", "zlib"],
                                env=env, check=True, text=True, capture_output=True, timeout=30)
        assert re.search(r"^zlib-", result.stdout, re.M), result.stdout + result.stderr
        result = subprocess.run(command + ["--initdb", "--simulate", "add", "hyfetch@testing"],
                                env=env, text=True, capture_output=True, timeout=30)
        assert result.returncode == 0, result.stdout + result.stderr
        assert re.search(r"Installing hyfetch@testing\b", result.stdout), result.stdout + result.stderr
        assert "WARNING" not in result.stderr and "ERROR" not in result.stderr, result.stderr
        assert not (root / "etc/apk/world").read_text().strip()
        assert not (root / "lib/apk/db/installed").exists()
        print("PASS upstream apk HTTPS + signed v3.24/edge testing indexes, cached search and HyFetch resolution (host Linux only)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--images", action="store_true", help="also inspect freshly built ext2 roots")
    parser.add_argument("--apk", type=Path, help="optional preverified upstream executable for disposable host reference")
    args = parser.parse_args()
    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(ApkSeedTests))
    if not result.wasSuccessful():
        raise SystemExit(1)
    if args.images:
        check_images()
    if args.apk:
        check_upstream(args.apk)
