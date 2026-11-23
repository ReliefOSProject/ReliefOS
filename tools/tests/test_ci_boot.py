#!/usr/bin/env python3
"""Boot acceptance observes the installer when exec diagnostics are disabled."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch


TOOLS = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("dual_boot_ci", TOOLS / "test_dual_boot_qemu.py")
boot = importlib.util.module_from_spec(spec)
# These waits need only the supplied probes. Keep the lightweight workflow
# checks independent of Pillow, QMP and a guest-image environment.
with patch.dict(sys.modules, {"test_installer_accounts_qemu": Mock(),
                              "test_vt_qemu": Mock()}):
    spec.loader.exec_module(boot)


class InstallerBootTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.serial = Path(temporary.name) / "serial.log"
        self.serial.write_text("[reliefnt] PID 1 path=/init\n[reliefnt] boot complete:\n")
        self.probe = Mock()
        self.process = Mock()
        self.process.poll.return_value = None

    def wait(self):
        with patch.object(boot.time, "monotonic", side_effect=(0, 0, 1, 2, 3, 5)), \
                patch.object(boot.time, "sleep"):
            boot.wait_installer_language(self.probe, self.serial, self.process, 4)

    def test_two_fresh_language_frames_pass_without_exec_debug_log(self):
        self.probe.visible_page.side_effect = (None, 0, None, 0, 0)
        with patch.object(boot.time, "monotonic", side_effect=(0, 0, 1, 2, 3, 4)), \
                patch.object(boot.time, "sleep"):
            boot.wait_installer_language(self.probe, self.serial, self.process, 5)
        self.assertEqual(self.probe.frame.call_count, 5)
        self.assertNotIn("installer.elf", self.serial.read_text())

    def test_exec_log_alone_or_wrong_page_cannot_pass(self):
        self.serial.write_text("path=/usr/lib/reliefos/apps/installer/installer.elf\n")
        for page in (None, 1, 10):
            with self.subTest(page=page):
                self.probe.visible_page.side_effect = None
                self.probe.visible_page.return_value = page
                with self.assertRaisesRegex(AssertionError, "language page did not appear"):
                    self.wait()

    def test_process_exit_or_kernel_panic_fails_even_if_page_is_visible(self):
        self.probe.visible_page.return_value = 0
        self.process.poll.return_value = 1
        with self.assertRaisesRegex(RuntimeError, "QEMU exited"):
            self.wait()
        self.process.poll.return_value = None
        for message in ("KERNEL PANIC", "Kernel Panic"):
            self.serial.write_text(message)
            with self.assertRaisesRegex(AssertionError, "Kernel panic"):
                self.wait()


if __name__ == "__main__":
    unittest.main()
