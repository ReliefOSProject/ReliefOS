"""Canonical package paths must participate in validation and license checks."""
from pathlib import Path
import tempfile
import unittest

from build_api import validate_install_path
from check_licenses import check_image
from apk_distribution import EXTERNAL
from make_installer_iso import stage_installer_tree


class PackagingPathsTests(unittest.TestCase):
    def test_media_payloads_are_excluded_from_packages(self):
        self.assertTrue({"EFI", "grub", "reliefos", "leonos", "install"} <= EXTERNAL)
        script = (Path(__file__).parent / "build/apk-stage.sh").read_text()
        self.assertEqual(script.count("for name in EFI grub reliefos leonos install;"), 2)

    def test_reference_iso_stages_canonical_and_rollback_payloads(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-iso-layout-") as directory:
            work = Path(directory)
            inputs = []
            for name in ("efi.img", "BOOTX64.EFI", "kernel.sys", "root.ext2", "font.pf2", "grub.cfg"):
                path = work / name
                path.write_bytes(name.encode())
                inputs.append(path)
            stage = work / "stage"
            stage_installer_tree(stage, *inputs)
            for namespace in ("reliefos", "leonos"):
                self.assertEqual((stage / namespace / "kernel.sys").read_bytes(), b"kernel.sys")
                self.assertTrue((stage / (namespace + "-installer-iso.marker")).is_file())

    def test_api_accepts_canonical_and_legacy_paths_but_rejects_traversal(self):
        for path in ("/usr/lib/reliefos/apps/demo", "/usr/lib/leonos/apps/demo", "/opt/demo", "/programs/demo"):
            validate_install_path(path)
        for path in ("/usr/lib/reliefos/apps/../etc", "/etc/demo", "/usr/lib/reliefos/apps//demo"):
            with self.assertRaises(ValueError):
                validate_install_path(path)

    def test_canonical_application_requires_its_license(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-license-") as directory:
            root = Path(directory)
            binary = root / "usr/lib/reliefos/apps/sl/sl.elf"
            binary.parent.mkdir(parents=True)
            binary.write_bytes(b"fixture")
            findings = [finding for finding in check_image(root) if finding.component == "sl"]
            self.assertEqual(len(findings), 1)
            self.assertEqual(findings[0].status, "fail")
            license_file = root / "usr/share/licenses/sl/LICENSE"
            license_file.parent.mkdir(parents=True)
            license_file.write_text("fixture license")
            findings = [finding for finding in check_image(root) if finding.component == "sl"]
            self.assertEqual(findings[0].status, "pass")


if __name__ == "__main__":
    unittest.main()
