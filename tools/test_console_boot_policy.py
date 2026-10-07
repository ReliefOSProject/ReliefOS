#!/usr/bin/env python3
"""Check that all boot environments use six ordinary VT sessions."""
from pathlib import Path
import unittest
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
ROOTFS = ROOT / "system/rootfs"


class ConsoleBootPolicyTests(unittest.TestCase):
    def test_console_cursor_and_background_output(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-vt-console-") as tmp:
            binary = str(Path(tmp) / "console")
            subprocess.run(["clang", "-std=c11", "-g", "-O1",
                            "-ffunction-sections", "-fdata-sections", "-fsanitize=address,undefined",
                            "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/kernel/reliefnt/include",
                            "-Wl,--gc-sections", "tools/tests/vt_console_test.c", "-o", binary],
                           cwd=ROOT, check=True)
            subprocess.run([binary], check=True, timeout=10)

    def test_init_respawns_a_session_on_each_virtual_terminal(self):
        config = (ROOTFS / "etc/inittab").read_text()
        for number in range(1, 7):
            self.assertIn(
                f"tty{number}::respawn:/usr/lib/reliefos/console-session tty{number}",
                config,
            )
        self.assertEqual(config.count("::respawn:"), 6)

    def test_console_session_restores_login_after_graphical_exit(self):
        script = (ROOTFS / "usr/lib/reliefos/console-session").read_text()
        self.assertIn("login.elf --graphical-session", script)
        self.assertIn("/run/reliefos/graphical-session-started", script)
        self.assertIn("login.elf --installer-shell", script)
        self.assertIn("exec /sbin/getty -n -l", script)
        self.assertLess(script.index("login.elf --graphical-session"),
                        script.index("exec /sbin/getty"))
        self.assertNotIn("LEONOS_BOOT_MODE", script)
        self.assertNotIn("RELIEFOS_BOOT_MODE", script)
        self.assertNotIn("/bin/sleep", script)

    def test_xorg_session_is_selected_by_raw_marker(self):
        script = (ROOTFS / "usr/lib/reliefos/console-session").read_text()
        self.assertIn("/etc/reliefos/desktop-backend", script)
        self.assertIn("reliefos-xdm", (ROOT / "system/xorg/reliefos-xdm").read_text())
        self.assertIn("xdm -nodaemon -config", (ROOT / "system/xorg/reliefos-xdm").read_text())
        self.assertNotIn("source ", script)
        self.assertNotIn(". /etc/reliefos/desktop-backend", script)
        self.assertNotIn("-novtswitch", (ROOT / "system/xorg/xdm-Xservers").read_text())

    def test_xorg_server_is_attached_to_tty1_before_vt_initialization(self):
        servers = (ROOT / "system/xorg/xdm-Xservers").read_text()
        wrapper = (ROOT / "system/xorg/xorg-tty-wrapper").read_text()
        self.assertIn("/usr/lib/reliefos/xorg-tty-wrapper", servers)
        self.assertIn("exec <\"$tty\" >\"$tty\" 2>&1", wrapper)
        self.assertIn("exec /usr/bin/Xorg \"$@\"", wrapper)
        self.assertNotIn("setsid", wrapper)

    def test_invalid_backend_marker_never_falls_back_to_native_desktop(self):
        script = (ROOTFS / "usr/lib/reliefos/console-session").read_text()
        self.assertIn("exactly one line", script)
        self.assertIn("invalid desktop backend", script)
        self.assertNotIn("desktop_backend=reliefos", script)

    def test_xorg_evidence_events_are_safe_and_orderable(self):
        launcher = (ROOT / "system/xorg/reliefos-xdm").read_text()
        session = (ROOT / "system/xorg/xdm-session").read_text()
        console = (ROOTFS / "usr/lib/reliefos/console-session").read_text()
        for event in ("desktop-backend=xorg", "xdm started on vt1"):
            self.assertIn(event, launcher)
        for event in ("PAM authentication accepted", "icewm started for uid=", "xterm started", "xdm session ended"):
            self.assertIn(event, session)
        self.assertIn("/var/log/xdm.log", launcher)
        self.assertIn("state_dir=/run/reliefos/xdm", launcher)
        self.assertIn("sink=$state_dir/events", launcher)
        self.assertIn("RELIEFOS_XORG_EVENT_FD=3", launcher)
        self.assertIn('>&"$event_fd"', session)
        self.assertNotIn("/run/reliefos/xorg-events.log", launcher + session)
        self.assertNotIn("chmod 0622", launcher)
        self.assertIn("xdm exit status=", launcher)
        self.assertIn("tty1 restored to text login", console)
        self.assertNotIn("password", launcher.lower() + session.lower())

    def test_graphical_and_installer_sessions_claim_a_controlling_terminal(self):
        source = (ROOT / "userland/apps/login/main.c").read_text()
        for item in ("setsid()", "TIOCSCTTY", "tcsetpgrp", "VT_ACTIVATE",
                     "KDSETMODE", "KD_GRAPHICS", "KD_TEXT"):
            self.assertIn(item, source)

    def test_runlevels_do_not_start_the_desktop_as_a_service(self):
        runlevels = ROOTFS / "etc/runlevels"
        for name in ("default", "installer"):
            self.assertFalse((runlevels / name / "leonos-desktop").exists())
            self.assertFalse((runlevels / name / "reliefos-desktop").exists())
        self.assertFalse((runlevels / "tty").exists())
        self.assertFalse((runlevels / "installer-tty").exists())
        self.assertTrue((ROOTFS / "etc/reliefos/desktop-session").exists())
        self.assertIn("installer-runtime", (ROOTFS / "usr/lib/reliefos/rc-default").read_text())

    def test_grub_uses_only_installer_session_selection(self):
        configs = [ROOT / f"boot/grub/{name}.cfg" for name in ("grub", "live", "installer")]
        for config in configs:
            self.assertNotIn("startup=", config.read_text())
        self.assertIn("installer-session=tui", configs[-1].read_text())


if __name__ == "__main__":
    unittest.main()
