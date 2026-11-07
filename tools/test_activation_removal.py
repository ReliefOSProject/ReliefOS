#!/usr/bin/env python3
"""Check the retired activation contract in OS sources and rebuilt products."""
import argparse
from pathlib import Path
import re
import subprocess
import tarfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
RETIRED = re.compile(
    r"\b(?:RELIEFOS|LEONOS)_(?:LICENSE_[A-Z0-9_]+|PATH_LICENSE)\b|"
    r"\b(?:reliefos|leonos)_license_\w+\b|"
    r"\b(?:K?CONFIG_)?(?:VMDK_REQUIRE_LICENSE|INSTALLER_INSTALLED_REQUIRE_LICENSE|"
    r"LICENSE_SERVER_URL|LICENSE_DEBUG_LOG)\b|"
    r"\b(?:installed_system_requires_license|license_server_url|license_debug_log)\b|"
    r"\b(?:AUTOCONF_INSTALLER_H|RELIEFOS_POLICY_HEADER|PAGE_ACTIVATION)\b|"
    r"LeonOS4 (?:local activation|offline license|machine id)|"
    r"/etc/(?:license\.conf|install\.id)\b|\blicense\.(?:dat|o)\b|"
    r'\bmsgid "(?:Activated|Activation|Activation denied|Activation mode|'
    r'Email hash|Invalid activation|License file|Not activated|'
    r'License validation is disabled for this build\.)"'
)
OUT = None


class SourceTests(unittest.TestCase):
    def test_retired_entry_points_are_removed(self):
        for name in ("userland/runtime/src/license.c", "include/reliefos/license.h",
                     "include/leonos/license.h", "userland/runtime/include/build_policy.h"):
            self.assertFalse((ROOT / name).exists(), name)

    def test_shipped_sources_have_no_activation_contract(self):
        files = subprocess.check_output(
            ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
            cwd=ROOT).decode().split("\0")
        findings = []
        for name in files:
            if (name.startswith(("third_party/", "kernel/", "LICENSES/"))
                    or name in {"tests/fixtures/brand-abi/old-sdk-exports.tsv",
                                "tests/fixtures/brand-abi/old-shared-symbols.tsv"}
                    or name == str(Path(__file__).relative_to(ROOT))):
                continue
            path = ROOT / name
            if not name or not path.is_file():
                continue
            data = path.read_bytes()
            if b"\0" in data[:4096]:
                continue
            for line, text in enumerate(data.decode(errors="replace").splitlines(), 1):
                if RETIRED.search(text):
                    findings.append(f"{name}:{line}")
        self.assertEqual(findings, [], "retired activation references: " + ", ".join(findings[:12]))


class ProductTests(unittest.TestCase):
    def setUp(self):
        if OUT is None:
            self.skipTest("native output is checked when --output is provided")

    def test_runtime_and_sdk_have_no_activation_exports_or_archive_members(self):
        for base in (OUT / "system/lib", OUT / "musl/lib",
                     OUT / "sdk/reliefos-musl-sdk/lib"):
            for library in ("libreliefos", "libleonos"):
                suffix = ".a" if base == OUT / "musl/lib" else ".so.2"
                path = base / (library + suffix)
                self.assertTrue(path.is_file(), str(path))
                command = ["nm", "--defined-only", str(path)]
                if suffix != ".a":
                    command.insert(1, "-D")
                symbols = subprocess.check_output(command, text=True, stderr=subprocess.DEVNULL)
                self.assertNotRegex(symbols, r"\b(?:reliefos|leonos)_license_\w+\b", str(path))
                data = path.read_bytes()
                for marker in (b"LeonOS4 local activation", b"LeonOS4 offline license",
                               b"LeonOS4 machine id"):
                    self.assertNotIn(marker, data, str(path))
            if base != OUT / "system/lib":
                for library in ("libreliefos.a", "libleonos.a"):
                    members = subprocess.check_output(["ar", "t", str(base / library)], text=True)
                    self.assertNotRegex(members, r"(?m)(?:^|/)license\.o$", str(base / library))

    def test_sdk_directory_and_archive_have_no_activation_headers(self):
        for namespace in ("reliefos", "leonos"):
            self.assertFalse((OUT / "sdk/reliefos-musl-sdk/include" / namespace / "license.h").exists())
        archives = list((OUT / "packages").glob("reliefos-musl-sdk.tar.gz"))
        self.assertEqual(len(archives), 1, "rebuilt SDK archive is required")
        with tarfile.open(archives[0]) as archive:
            self.assertFalse(any(name.endswith(("/reliefos/license.h", "/leonos/license.h"))
                                 for name in archive.getnames()))

    def test_generated_configuration_has_no_activation_policy(self):
        for name in (".config", "include/config/auto.conf", "include/generated/autoconf.h"):
            self.assertIsNone(RETIRED.search((OUT / name).read_text()), name)
        self.assertFalse((OUT / "include/generated/autoconf-installer.h").exists())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, help="also check rebuilt native output and SDK")
    args = parser.parse_args()
    OUT = args.output.resolve() if args.output else None
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(SourceTests)
    if OUT is not None:
        suite.addTests(unittest.defaultTestLoader.loadTestsFromTestCase(ProductTests))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if result.wasSuccessful() else 1)
