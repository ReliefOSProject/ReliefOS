#!/usr/bin/env python3
"""CI setup installs the selected suite's dependencies without touching the host."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
INSTALL = ROOT / "scripts/ci/install.sh"


class SetupTests(unittest.TestCase):
    def run_setup(self, *arguments, fail=None):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            log = directory / "calls.jsonl"
            sudo = directory / "sudo"
            sudo.write_text(f"#!{sys.executable}\n" +
                            "import json,os,sys\n"
                            "with open(os.environ['TEST_APT_LOG'],'a') as output:\n"
                            "    output.write(json.dumps(sys.argv[1:])+'\\n')\n"
                            "if len(sys.argv)>2 and sys.argv[2]==os.environ.get('TEST_APT_FAIL'):\n"
                            "    sys.exit(42)\n")
            sudo.chmod(0o755)
            environment = dict(os.environ, PATH=str(directory) + os.pathsep + os.environ["PATH"],
                               TEST_APT_LOG=str(log), TEST_APT_FAIL=fail or "")
            result = subprocess.run(["bash", str(INSTALL), *arguments], capture_output=True,
                                    text=True, env=environment, check=False)
            calls = [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []
            return result, calls

    def packages(self, profile):
        result, calls = self.run_setup(profile)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls[0], ["apt-get", "update"])
        self.assertEqual(calls[1][:4], ["apt-get", "install", "--yes", "--no-install-recommends"])
        self.assertEqual(len(calls), 2)
        packages = calls[1][4:]
        self.assertEqual(len(packages), len(set(packages)), "duplicate install requests")
        return set(packages)

    def test_focused_profiles_keep_compilers_native_tools_and_crypto(self):
        required = {"build-essential", "clang", "libclang-rt-dev", "llvm", "lld", "ccache",
                    "bison", "flex", "gperf", "gettext", "git", "make", "libncurses-dev",
                    "pkg-config", "autoconf", "automake", "libtool", "libtool-bin",
                    "openssl", "python3", "libext2fs-dev"}
        for profile in ("kernel", "auth", "pages"):
            with self.subTest(profile=profile):
                packages = self.packages(profile)
                self.assertTrue(required <= packages, required - packages)
                self.assertTrue(packages.isdisjoint({"ffmpeg", "libavformat-dev", "libavcodec-dev",
                                                     "grub-pc-bin", "grub-efi-amd64-bin",
                                                     "xvfb", "libx11-dev"}))

    def test_kernel_profile_supports_the_full_doctor_contract(self):
        image_tools = {"e2fsprogs", "squashfs-tools", "dosfstools", "mtools", "xorriso",
                       "qemu-utils", "qemu-system-x86", "grub-common", "fdisk", "fakeroot",
                       "zip", "ovmf"}
        self.assertTrue(image_tools <= self.packages("kernel"))
        for profile in ("auth", "pages"):
            self.assertTrue(image_tools.isdisjoint(self.packages(profile)))

    def test_only_pages_focused_profile_needs_sphinx(self):
        self.assertTrue({"python3-sphinx", "python3-pil"} <= self.packages("pages"))
        for profile in ("kernel", "auth"):
            self.assertNotIn("python3-sphinx", self.packages(profile))

    def test_full_build_and_smoke_keep_their_existing_runtime_requirements(self):
        self.assertTrue({"ffmpeg", "libavformat-dev", "libavcodec-dev", "libavutil-dev",
                         "libswresample-dev", "libswscale-dev", "qemu-system-x86", "ovmf",
                         "grub-common", "xvfb", "python3-sphinx"} <= self.packages("build"))
        self.assertEqual(self.packages("smoke"),
                         {"qemu-system-x86", "qemu-utils", "ovmf", "python3-pil", "tesseract-ocr"})

    def test_invalid_profile_never_runs_package_manager(self):
        for arguments in ((), ("unknown",), ("",)):
            result, calls = self.run_setup(*arguments)
            self.assertEqual(result.returncode, 2)
            self.assertEqual(calls, [])

    def test_update_and_install_failures_propagate(self):
        result, calls = self.run_setup("kernel", fail="update")
        self.assertEqual(result.returncode, 42)
        self.assertEqual(calls, [["apt-get", "update"]])
        result, calls = self.run_setup("pages", fail="install")
        self.assertEqual(result.returncode, 42)
        self.assertEqual(len(calls), 2)


if __name__ == "__main__":
    unittest.main()
