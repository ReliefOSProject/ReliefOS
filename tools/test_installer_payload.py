"""Run the real installer media validator against the staged dual ABI layout."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class InstallerPayloadTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # An explicit TMPDIR keeps compiler scratch and fixtures on the caller's data disk.
        cls.work = tempfile.TemporaryDirectory(
            prefix="reliefos-installer-payload-", dir=os.environ["TMPDIR"])
        cls.executable = Path(cls.work.name) / "payload-check"
        subprocess.run([
            "cc", "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
            "-idirafter", "include", "-Ikernel/reliefnt/include",
            "-Ikernel/reliefnt/include/uapi", "-idirafter", "userland/runtime/include",
            "tools/tests/installer_payload_test.c", "-o", str(cls.executable),
        ], cwd=ROOT, check=True)

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def setUp(self):
        self.fixture = tempfile.TemporaryDirectory(prefix="media-", dir=self.work.name)
        self.addCleanup(self.fixture.cleanup)
        self.root = Path(self.fixture.name)
        # Derived from rootfs-stage.sh and installer-stage.sh, not the validator's arrays.
        for name in (
            "etc/reliefos", "var/lib/reliefos", "usr/lib/reliefos/apps",
            "usr/share/doc/reliefos",
            "usr/share/fonts/reliefos", "usr/share/reliefos/resources",
            "usr/share/licenses", "etc/ssl/certs",
        ):
            (self.root / "install/root" / name).mkdir(parents=True, exist_ok=True)
        for name in (
            "root/usr/lib/reliefos/kerneldebug.sys", "root/lib/ld-musl-x86_64.so.1",
            "root/lib/libc.so", "root/lib/libmimalloc.so.3",
            "root/usr/lib/reliefos/libreliefos.so.2", "root/usr/lib/leonos/libleonos.so.2",
            "esp/reliefos/kernel.sys", "esp/reliefos/loader.elf", "esp/leonos/kernel.sys",
            "esp/loader.elf", "esp/grub/grub.cfg", "esp/EFI/BOOT/BOOTX64.EFI",
        ):
            target = self.root / "install" / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(b"staged payload\n")

    def validate(self):
        return subprocess.run([self.executable, self.root], cwd=ROOT, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)

    def target_fixture(self, namespace):
        root = self.root / "target"
        for name in ("bin", "sbin", "lib", "usr/bin", "usr/sbin", "usr/lib",
                     f"etc/{namespace}", f"var/lib/{namespace}",
                     f"usr/lib/{namespace}/apps", "boot/EFI"):
            (root / name).mkdir(parents=True, exist_ok=True)
        library = "libreliefos.so.2" if namespace == "reliefos" else "libleonos.so.2"
        for name in (f"usr/lib/{namespace}/{library}", "boot/loader.elf", f"boot/{namespace}/kernel.sys"):
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"old installed payload")
        return root

    def validate_target(self):
        return subprocess.run([self.executable, self.root, "target"], cwd=ROOT,
                              capture_output=True, text=True, timeout=10)

    def test_existing_canonical_install_is_accepted(self):
        self.target_fixture("reliefos")
        result = self.validate_target()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_existing_legacy_install_is_accepted_without_rewriting_it(self):
        root = self.target_fixture("leonos")
        result = self.validate_target()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((root / "etc/reliefos").exists())

    def test_usr_merge_legacy_install_remains_rejected(self):
        root = self.target_fixture("leonos")
        (root / "bin").rmdir()
        (root / "bin").symlink_to("usr/bin")
        self.assertEqual(self.validate_target().returncode, 1)

    def test_legacy_namespace_symlink_is_rejected(self):
        root = self.target_fixture("leonos")
        (root / "etc/leonos").rmdir()
        (root / "etc/leonos").symlink_to("../var/lib/leonos")
        self.assertEqual(self.validate_target().returncode, 1)

    def test_dual_runtime_media_is_accepted_without_so1(self):
        result = self.validate()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_canonical_runtime_is_required_even_with_misplaced_legacy(self):
        canonical = self.root / "install/root/usr/lib/reliefos/libreliefos.so.2"
        canonical.rename(canonical.with_name("libleonos.so.2"))
        result = self.validate()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("/install/root/usr/lib/reliefos/libreliefos.so.2", result.stdout)

    def test_legacy_runtime_is_required_for_existing_elf(self):
        (self.root / "install/root/usr/lib/leonos/libleonos.so.2").unlink()
        result = self.validate()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("/install/root/usr/lib/leonos/libleonos.so.2", result.stdout)

    def test_runtime_directory_does_not_count_as_library(self):
        canonical = self.root / "install/root/usr/lib/reliefos/libreliefos.so.2"
        canonical.unlink()
        canonical.mkdir()
        result = self.validate()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("payload-check=-20", result.stdout)

    def test_legacy_boot_payload_is_still_required(self):
        (self.root / "install/esp/leonos/kernel.sys").unlink()
        result = self.validate()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("/install/esp/leonos/kernel.sys", result.stdout)


if __name__ == "__main__":
    unittest.main()
