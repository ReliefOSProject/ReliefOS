#!/usr/bin/env python3
"""Run the real XDM launcher and session scripts against stub XDM/IceWM/st.

The fixtures execute the unmodified POSIX shell scripts inside a chroot built in
an unprivileged user namespace, so their absolute guest paths resolve normally
and the assertions cover the behaviour a booting tty1 would observe: the
launcher must merge the lifecycle events, record the server return code, drop
its scratch sink and hand tty1 back to the text getty on every exit path, and
the session must record PAM -> IceWM -> st -> session end in that order without
ever claiming an "append-only" file that the user could truncate.
"""
from pathlib import Path
import os
import shutil
import stat
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
XORG = ROOT / "system/xorg"
CONSOLE_SESSION = ROOT / "system/rootfs/usr/lib/reliefos/console-session"

COMMANDS = ["sh", "bash", "cp", "wc", "tr", "rm", "touch", "mkdir", "chmod",
            "cat", "head", "sleep", "stat", "printf", "awk", "grep"]


def resolve(command: str) -> Path:
    path = shutil.which(command)
    if not path:
        raise unittest.SkipTest(f"{command} is not available")
    return Path(path)


def copy_with_libs(target: Path, source: Path, dest_name: str | None = None) -> None:
    name = dest_name or source.name
    destination = target / "usr/bin" / name
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination, follow_symlinks=True)
    destination.chmod(0o755)
    ldd = subprocess.run(["ldd", str(source)], capture_output=True, text=True)
    for line in ldd.stdout.splitlines():
        parts = line.split()
        for token in parts:
            if token.startswith("/") and Path(token).exists():
                library = target / token.lstrip("/")
                library.parent.mkdir(parents=True, exist_ok=True)
                if not library.exists():
                    shutil.copy2(token, library, follow_symlinks=True)


def build_sandbox(work: Path) -> Path:
    sandbox = work / "root"
    for name in ("usr/bin", "usr/lib/reliefos", "etc/reliefos", "etc/X11",
                 "var/log", "var/lib/xdm", "run/reliefos", "home/tester", "tmp",
                 "dev", "proc", "root", "sbin"):
        (sandbox / name).mkdir(parents=True, exist_ok=True)
    # The scripts redirect diagnostics to /dev/null; a plain file is enough.
    (sandbox / "dev/null").write_text("")
    (sandbox / "proc/cmdline").write_text("")
    for command in COMMANDS:
        copy_with_libs(sandbox, resolve(command))
    # /bin/sh is what the scripts' shebangs name.
    bin_dir = sandbox / "bin"
    bin_dir.mkdir(parents=True, exist_ok=True)
    shell = resolve("sh")
    shutil.copy2(shell, bin_dir / "sh", follow_symlinks=True)
    (bin_dir / "sh").chmod(0o755)
    ldd = subprocess.run(["ldd", str(shell)], capture_output=True, text=True)
    for line in ldd.stdout.splitlines():
        for token in line.split():
            if token.startswith("/") and Path(token).exists():
                library = sandbox / token.lstrip("/")
                library.parent.mkdir(parents=True, exist_ok=True)
                if not library.exists():
                    shutil.copy2(token, library, follow_symlinks=True)
    shutil.copy2(XORG / "reliefos-xdm", sandbox / "usr/lib/reliefos/reliefos-xdm")
    shutil.copy2(XORG / "xorg-tty-wrapper", sandbox / "usr/lib/reliefos/xorg-tty-wrapper")
    shutil.copy2(XORG / "xdm-session", sandbox / "usr/lib/reliefos/xdm-session")
    (sandbox / "usr/lib/reliefos/reliefos-xdm").chmod(0o755)
    (sandbox / "usr/lib/reliefos/xorg-tty-wrapper").chmod(0o755)
    (sandbox / "usr/lib/reliefos/xdm-session").chmod(0o755)
    shutil.copy2(CONSOLE_SESSION, sandbox / "usr/lib/reliefos/console-session")
    (sandbox / "usr/lib/reliefos/console-session").chmod(0o755)
    (sandbox / "etc/reliefos/xdm.conf").write_text("DisplayManager._0: :0\n")
    (sandbox / "etc/reliefos/xdm-Xservers").write_text(
        (XORG / "xdm-Xservers").read_text())
    (sandbox / "etc/reliefos/xdm-Xresources").write_text(
        (XORG / "xdm-Xresources").read_text())
    (sandbox / "etc/X11/xorg.conf").write_text("Section \"Device\"\nEndSection\n")
    (sandbox / "etc/reliefos/icewm").mkdir(parents=True, exist_ok=True)
    (sandbox / "etc/reliefos/icewm/preferences").write_text("Theme=\"light/light.theme\"\n")
    return sandbox


def write_stub(path: Path, body: str) -> None:
    path.write_text("#!/bin/sh\n" + body)
    path.chmod(0o755)


class XdmLauncherTests(unittest.TestCase):
    def setUp(self):
        self.work = Path(tempfile.mkdtemp(prefix="reliefos-xdm-launcher-"))
        self.sandbox = build_sandbox(self.work)
        self.addCleanup(shutil.rmtree, self.work, ignore_errors=True)

    def run_guest(self, script: str, timeout: int = 30) -> subprocess.CompletedProcess:
        command = ["unshare", "--user", "--map-root-user", "--mount",
                   "chroot", str(self.sandbox), "/bin/sh", "-c", script]
        return subprocess.run(command, capture_output=True, text=True,
                              env={**os.environ, "LC_ALL": "C"},
                              timeout=timeout)

    def log(self) -> str:
        return (self.sandbox / "var/log/xdm.log").read_text()

    def events(self) -> list[str]:
        return [line.strip() for line in self.log().splitlines()]

    def install_session_stubs(self, uid: str = "1000",
                              wm: str = "exec sleep 2\n",
                              st: str = "exec sleep 4\n") -> None:
        write_stub(self.sandbox / "usr/bin/id", f'if test "$1" = -u; then echo {uid}; else echo tester; fi\n')
        # The stub records its configuration environment so the session can be
        # held to leaving IceWM's personal configuration path unassigned.  No
        # twm stub exists: a session that still execs /usr/bin/twm fails.
        write_stub(self.sandbox / "usr/bin/icewm",
                   'printf \'%s\\n\' "$ICEWM_PRIVCFG" > /run/icewm.privcfg\n' + wm)
        write_stub(self.sandbox / "usr/bin/st", st)

    def wm_config(self) -> str:
        return (self.sandbox / "run/icewm.privcfg").read_text()

    def install_xdm(self, status: int, run_session: bool = True) -> None:
        # Real xdm runs the session script and then terminates with its own
        # status: a non-zero server status wins, otherwise the session's status
        # is what the launcher must report.
        if run_session:
            body = (
                "export HOME=/home/tester DISPLAY=:0\n"
                "/usr/lib/reliefos/xdm-session\n"
                "session_status=$?\n"
                f"if test {status} -ne 0; then exit {status}; fi\n"
                "exit $session_status\n")
        else:
            body = f"exit {status}\n"
        write_stub(self.sandbox / "usr/bin/xdm", body)

    def install_console_session(self, marker: str,
                                installer_runtime: bool = False) -> None:
        (self.sandbox / "etc/reliefos/desktop-backend").write_text(marker)
        runtime_marker = self.sandbox / "etc/reliefos/installer-runtime"
        if installer_runtime:
            runtime_marker.write_text("installer\n")
        else:
            runtime_marker.unlink(missing_ok=True)
        write_stub(
            self.sandbox / "sbin/getty",
            'printf \'%s\\n\' "$*" >> /run/getty.log\n',
        )
        self.install_xdm(0, run_session=False)

    def run_console_session(self, tty: str = "tty1") -> subprocess.CompletedProcess:
        return self.run_guest(f"exec /usr/lib/reliefos/console-session {tty}")

    def test_console_backend_uses_text_getty_without_graphical_startup(self):
        self.install_console_session("console\n")

        result = self.run_console_session()

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.sandbox / "run/getty.log").read_text().splitlines(),
                         ["-n -l /usr/lib/reliefos/apps/login/login.elf 115200 tty1"])
        self.assertNotIn("invalid desktop backend", result.stderr)
        self.assertFalse((self.sandbox / "run/reliefos/graphical-session-started").exists())
        self.assertFalse((self.sandbox / "var/log/xdm.log").exists())

    def test_console_backend_hands_tty1_directly_to_getty_with_installer_runtime(self):
        self.install_console_session("console\n", installer_runtime=True)

        result = self.run_console_session()

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.sandbox / "run/getty.log").read_text().splitlines(),
                         ["-n -l /usr/lib/reliefos/apps/login/login.elf 115200 tty1"])
        self.assertNotIn("invalid desktop backend", result.stderr)
        self.assertFalse((self.sandbox / "run/reliefos/graphical-session-started").exists())
        self.assertFalse((self.sandbox / "var/log/xdm.log").exists())

    def test_invalid_and_multiline_backend_markers_fall_back_to_text_getty(self):
        for marker in ("wayland\n", "console\nxorg\n", ""):
            with self.subTest(marker=marker):
                self.install_console_session(marker)

                result = self.run_console_session()

                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("invalid desktop backend", result.stderr)
                self.assertEqual(len((self.sandbox / "run/getty.log").read_text().splitlines()), 1)
                self.assertFalse((self.sandbox / "run/reliefos/graphical-session-started").exists())
                self.assertFalse((self.sandbox / "var/log/xdm.log").exists())
                (self.sandbox / "run/getty.log").unlink()

    def test_xorg_backend_keeps_the_graphical_then_text_handoff(self):
        self.install_console_session("xorg\n")

        result = self.run_console_session()

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((self.sandbox / "run/reliefos/graphical-session-started").exists())
        self.assertEqual((self.sandbox / "var/log/xdm.log").read_text().splitlines()[-1],
                         "reliefos-xdm: tty1 restored to text login")
        self.assertEqual((self.sandbox / "run/getty.log").read_text().splitlines(),
                         ["-n -l /usr/lib/reliefos/apps/login/login.elf 115200 tty1"])

    def test_tty2_through_tty6_remain_text_gettys(self):
        self.install_console_session("console\n")

        for number in range(2, 7):
            result = self.run_console_session(f"tty{number}")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        self.assertEqual((self.sandbox / "run/getty.log").read_text().splitlines(), [
            f"-n -l /usr/lib/reliefos/apps/login/login.elf 115200 tty{number}"
            for number in range(2, 7)
        ])
        self.assertFalse((self.sandbox / "run/reliefos/graphical-session-started").exists())
        self.assertFalse((self.sandbox / "var/log/xdm.log").exists())

    def test_success_merges_events_records_status_and_restores_getty(self):
        self.install_session_stubs()
        self.install_xdm(0)
        result = self.run_guest("exec /usr/lib/reliefos/reliefos-xdm")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        log = self.log()
        self.assertIn("desktop-backend=xorg", log)
        self.assertIn("xdm started on vt1", log)
        self.assertIn("xdm exit status=0", log)
        self.assertIn("tty1 restored to text login", log)
        self.assertFalse((self.sandbox / "run/reliefos/xdm/events").exists(),
                         "the temporary evidence sink must be removed")
        # The real server return code is what console-session observes.
        self.assertIn("PAM authentication accepted", log)
        self.assertIn("icewm started for uid=1000", log)
        self.assertIn("st started", log)
        self.assertIn("xdm session ended", log)

    def test_failure_still_merges_events_and_reports_the_return_code(self):
        self.install_session_stubs()
        self.install_xdm(1)
        result = self.run_guest("exec /usr/lib/reliefos/reliefos-xdm")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        log = self.log()
        self.assertIn("xdm exit status=1", log)
        self.assertIn("tty1 restored to text login", log)
        self.assertFalse((self.sandbox / "run/reliefos/xdm/events").exists())

    def test_events_follow_the_real_session_lifecycle(self):
        self.install_session_stubs()
        self.install_xdm(0)
        result = self.run_guest("exec /usr/lib/reliefos/reliefos-xdm")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        lines = self.events()
        wanted = ["PAM authentication accepted",
                  "icewm started for uid=1000",
                  "st started",
                  "xdm session ended"]
        positions = [lines.index(item) for item in wanted]
        self.assertEqual(positions, sorted(positions),
                         f"event order is {lines}")
        # A launch attempt must never be recorded before the child exists.
        self.assertNotIn("icewm failed to start", "\n".join(lines))
        self.assertLess(lines.index("icewm started for uid=1000"),
                        lines.index("st started"))
        # The session must let IceWM use personal configuration before the
        # standard system defaults rather than force a root-owned directory.
        self.assertEqual(self.wm_config().strip(),
                         os.environ.get("ICEWM_PRIVCFG", ""))

    def test_immediate_child_death_is_not_recorded_as_started(self):
        self.install_session_stubs(wm="exit 3\n")
        self.install_xdm(0)
        result = self.run_guest("exec /usr/lib/reliefos/reliefos-xdm")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        log = self.log()
        self.assertIn("icewm failed to start", log)
        self.assertNotIn("icewm started for uid=", log)
        self.assertNotIn("st started", log)
        self.assertIn("tty1 restored to text login", log)

    def test_session_runs_icewm_and_st_for_root_after_authentication(self):
        self.install_session_stubs(uid="0")
        self.install_xdm(0)
        result = self.run_guest("exec /usr/lib/reliefos/reliefos-xdm")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        log = self.log()
        self.assertIn("icewm started for uid=0", log)
        self.assertIn("st started", log)

    def test_window_manager_exit_terminates_the_remaining_terminal(self):
        self.install_session_stubs(wm="exec sleep 1\n",
                                   st="echo $$ > /run/st.pid\nexec sleep 4\n")
        self.install_xdm(0)
        started = time.monotonic()
        result = self.run_guest("exec /usr/lib/reliefos/reliefos-xdm")
        elapsed = time.monotonic() - started
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertLess(elapsed, 3, "logout must not wait for a running st")
        pid = int((self.sandbox / "run/st.pid").read_text())
        probe = self.run_guest(f"kill -0 {pid} 2>/dev/null")
        self.assertNotEqual(probe.returncode, 0, "st must be terminated and reaped")
        self.assertIn("xdm session ended", self.log())

    def test_missing_xdm_reports_the_stage_and_restores_getty(self):
        self.install_session_stubs()
        self.install_xdm(0)
        (self.sandbox / "usr/bin/xdm").unlink()
        result = self.run_guest("exec /usr/lib/reliefos/reliefos-xdm")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        log = self.log()
        self.assertIn("missing /usr/bin/xdm", log)
        self.assertIn("tty1 restored to text login", log)
        self.assertFalse((self.sandbox / "run/reliefos/xdm/events").exists())

    def test_root_log_is_not_readable_or_truncatable_by_the_session(self):
        self.install_session_stubs()
        self.install_xdm(0)
        result = self.run_guest("exec /usr/lib/reliefos/reliefos-xdm")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        log_path = self.sandbox / "var/log/xdm.log"
        mode = stat.S_IMODE(log_path.stat().st_mode)
        self.assertEqual(mode, 0o600, "the root record must be root-only")
        # The scratch sink lived in a root-only directory and is gone now.
        state_dir = self.sandbox / "run/reliefos/xdm"
        self.assertFalse((state_dir / "events").exists())
        # No script may promise append-only semantics it cannot enforce.
        for script in ("reliefos-xdm", "xdm-session"):
            text = (self.sandbox / "usr/lib/reliefos" / script).read_text()
            self.assertNotIn("append-only", text.lower())
            self.assertNotIn("0622", text)
            self.assertNotIn("password", text.lower())
            self.assertNotIn("cookie", text.lower())

    def test_launcher_writes_the_first_xorg_log_once(self):
        self.install_session_stubs()
        self.install_xdm(0)
        (self.sandbox / "var/log/Xorg.0.log").write_text("first fatal error\n")
        result = self.run_guest("exec /usr/lib/reliefos/reliefos-xdm")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.sandbox / "var/log/Xorg.0.log.first").read_text(),
                         "first fatal error\n")
        self.assertIn("xorg log present bytes=", self.log())

    def test_launcher_preserves_a_log_created_during_the_failed_launch(self):
        write_stub(self.sandbox / "usr/bin/xdm",
                   "printf \"first fatal error\\n\" > /var/log/Xorg.0.log\nexit 1\n")
        result = self.run_guest("exec /usr/lib/reliefos/reliefos-xdm")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        saved = self.sandbox / "var/log/Xorg.0.log.first"
        self.assertTrue(saved.is_file(), "preserve the log from this first launch")
        self.assertEqual(saved.read_text(), "first fatal error\n")
        write_stub(self.sandbox / "usr/bin/xdm",
                   "printf \"later fatal error\\n\" > /var/log/Xorg.0.log\nexit 1\n")
        result = self.run_guest("exec /usr/lib/reliefos/reliefos-xdm")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(saved.read_text(), "first fatal error\n")

    def test_console_session_hands_off_once_after_xdm(self):
        self.install_session_stubs()
        self.install_xdm(0)
        script = (self.sandbox / "usr/lib/reliefos/console-session").read_text()
        self.assertIn("/usr/lib/reliefos/reliefos-xdm", script)
        self.assertIn("tty1 restored to text login", script)
        # The text getty handoff execs, so it is the single final owner of tty1.
        self.assertIn("exec \\\n        /sbin/getty", script)
        self.assertEqual(script.rstrip().splitlines()[-1], "start_getty \"$1\"")
        # The graphical session runs in the foreground of this console session,
        # so no second getty or shell can own tty1 at the same time.
        self.assertLess(script.index("reliefos-xdm"), script.index("tty1 restored to text login"))
        self.assertNotIn("set -e", (self.sandbox / "usr/lib/reliefos/reliefos-xdm").read_text())

    def test_xorg_server_command_uses_the_tty_wrapper(self):
        servers = (XORG / "xdm-Xservers").read_text()
        wrapper = (XORG / "xorg-tty-wrapper").read_text()
        self.assertIn("/usr/lib/reliefos/xorg-tty-wrapper", servers)
        self.assertIn("exec <\"$tty\" >\"$tty\" 2>&1", wrapper)
        self.assertIn("exec /usr/bin/Xorg \"$@\"", wrapper)


if __name__ == "__main__":
    unittest.main()
