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

COMMANDS = ["sh", "bash", "env", "cp", "wc", "tr", "rm", "touch", "mkdir", "chmod",
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
    # Only XDM's standard config and script paths are installed.  The full
    # lifecycle tests therefore exercise login and logout with every former
    # ReliefOS-specific XDM path absent.
    for name in ("usr/bin", "usr/sbin", "usr/lib/reliefos", "usr/lib/X11/xdm",
                 "etc/reliefos", "etc/X11/xdm", "var/log", "var/lib/xdm",
                 "run/reliefos", "home/tester", "tmp",
                 "dev", "proc", "root", "sbin"):
        (sandbox / name).mkdir(parents=True, exist_ok=True)
    # The scripts redirect diagnostics to /dev/null; a plain file is enough.
    (sandbox / "dev/null").write_text("")
    (sandbox / "dev/tty1").write_text("")
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
    (sandbox / "usr/lib/reliefos/reliefos-xdm").chmod(0o755)
    (sandbox / "usr/lib/reliefos/xorg-tty-wrapper").chmod(0o755)
    write_stub(sandbox / "usr/bin/Xorg", "exit 0\n")
    shutil.copy2(XORG / "xdm-session", sandbox / "usr/lib/X11/xdm/Xsession")
    shutil.copy2(XORG / "xdm-setup", sandbox / "usr/lib/X11/xdm/Xsetup_0")
    (sandbox / "usr/lib/X11/xdm/Xsession").chmod(0o755)
    (sandbox / "usr/lib/X11/xdm/Xsetup_0").chmod(0o755)
    shutil.copy2(CONSOLE_SESSION, sandbox / "usr/lib/reliefos/console-session")
    (sandbox / "usr/lib/reliefos/console-session").chmod(0o755)
    shutil.copy2(XORG / "xdm.conf", sandbox / "etc/X11/xdm/xdm-config")
    shutil.copy2(XORG / "xdm-Xservers", sandbox / "etc/X11/xdm/Xservers")
    shutil.copy2(XORG / "xdm-Xresources", sandbox / "etc/X11/xdm/Xresources")
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

    def run_guest_with_fbdev(self, script: str, drm: bool = False) -> subprocess.CompletedProcess:
        framebuffer = self.sandbox / "dev/fb0"
        framebuffer.touch()
        mounts = 'mount --bind /dev/zero "$1/dev/fb0"'
        if drm:
            drm_node = self.sandbox / "dev/dri/card0"
            drm_node.parent.mkdir(parents=True, exist_ok=True)
            drm_node.touch()
            mounts += ' && mount --bind /dev/zero "$1/dev/dri/card0"'
        command = ["unshare", "--user", "--map-root-user", "--mount", "sh", "-c",
                   mounts + ' && chroot "$1" /bin/sh -c "$2"',
                   "sh", str(self.sandbox), script]
        return subprocess.run(command, capture_output=True, text=True,
                              env={**os.environ, "LC_ALL": "C"}, timeout=30)

    def log(self) -> str:
        return (self.sandbox / "var/log/xdm.log").read_text()

    def events(self) -> list[str]:
        return [line.strip() for line in self.log().splitlines()]

    def install_session_stubs(self, uid: str = "1000",
                              wm: str = "exec sleep 2\n",
                              st: str = "exec sleep 4\n") -> None:
        write_stub(self.sandbox / "usr/bin/id", f'if test "$1" = -u; then echo {uid}; else echo tester; fi\n')
        # Model IceWM's native session supervisor: Xsession must start
        # icewm-session, which then execs the window manager process.
        write_stub(self.sandbox / "usr/bin/icewm-session",
                   'printf \'%s\\n\' started >> /run/icewm-session.log\n'
                   'exec /usr/bin/icewm\n')
        # The WM stub records its configuration environment so the session can
        # be held to leaving IceWM's personal configuration path unassigned.
        write_stub(self.sandbox / "usr/bin/icewm",
                   'printf \'%s\\n\' "$ICEWM_PRIVCFG" > /run/icewm.privcfg\n' + wm)
        write_stub(self.sandbox / "usr/bin/st", st)

    def install_wallpaper_probe(self) -> None:
        (self.sandbox / "etc/reliefos/wallpaper.png").write_bytes(b"fixture")
        write_stub(self.sandbox / "usr/bin/xwallpaper",
                   'printf \'%s\\n\' "$*" >> /run/xwallpaper.calls\n')

    def wm_config(self) -> str:
        return (self.sandbox / "run/icewm.privcfg").read_text()

    def install_xdm(self, status: int, run_session: bool = True) -> None:
        # XDM's user environment contains its fixed session values plus names
        # from the installed DisplayManager.exportList.  This fixture models
        # only that exact, whitespace-separated resource subset; it is not a
        # general X resource parser.  env -i filters the launcher environment
        # while preserving inherited descriptors such as the event sink on 3.
        if run_session:
            body = (
                'printf \'%s\\n\' "$@" > /run/xdm.argv\n'
                'export HOME=/home/tester DISPLAY=:0 LOGNAME=tester USER=tester SHELL=/bin/sh\n'
                'export PATH=/usr/bin:/bin\n'
                'export_list=$(/usr/bin/awk \'\n'
                '  /^[[:space:]]*DisplayManager[.]exportList[[:space:]]*:/ {\n'
                '    sub(/^[^:]*:[[:space:]]*/, "")\n'
                '    print\n'
                '    exit\n'
                '  }\' /etc/X11/xdm/xdm-config)\n'
                'set -- HOME=/home/tester DISPLAY=:0 LOGNAME=tester USER=tester '
                'SHELL=/bin/sh PATH=/usr/bin:/bin\n'
                'if test "${TZ+x}" = x; then set -- "$@" "TZ=$TZ"; fi\n'
                'for exported_name in $export_list; do\n'
                '  case "$exported_name" in\n'
                '    RELIEFOS_XORG_EVENT_FD)\n'
                '      set -- "$@" "RELIEFOS_XORG_EVENT_FD=$RELIEFOS_XORG_EVENT_FD"\n'
                '      ;;\n'
                '  esac\n'
                'done\n'
                '/usr/bin/env -i "$@" /usr/lib/X11/xdm/Xsession\n'
                "session_status=$?\n"
                f"if test {status} -ne 0; then exit {status}; fi\n"
                "exit $session_status\n")
        else:
            body = 'printf \'%s\\n\' "$@" > /run/xdm.argv\n' + f"exit {status}\n"
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
        self.assertEqual((self.sandbox / "run/xdm.argv").read_text().splitlines(),
                         ["-nodaemon"])
        self.assertIn("xdm exit status=0", log)
        self.assertIn("tty1 restored to text login", log)
        self.assertFalse((self.sandbox / "run/reliefos/xdm/events").exists(),
                         "the temporary evidence sink must be removed")
        # The real server return code is what console-session observes.
        self.assertIn("PAM authentication accepted", log)
        self.assertIn("icewm started for uid=1000", log)
        self.assertEqual((self.sandbox / "run/icewm-session.log").read_text().splitlines(),
                         ["started"])
        self.assertIn("st started", log)
        self.assertIn("xdm session ended", log)

    def test_xsession_does_not_force_the_wallpaper(self):
        self.install_session_stubs()
        self.install_wallpaper_probe()
        self.install_xdm(0)

        result = self.run_guest("exec /usr/lib/reliefos/reliefos-xdm")

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((self.sandbox / "run/xwallpaper.calls").exists())

    def test_xsetup_is_a_noop_for_wallpaper(self):
        self.install_wallpaper_probe()

        result = self.run_guest("exec /usr/lib/X11/xdm/Xsetup_0")

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((self.sandbox / "run/xwallpaper.calls").exists())

    def test_missing_standard_xdm_config_is_reported_before_start(self):
        self.install_session_stubs()
        self.install_xdm(0)
        (self.sandbox / "etc/X11/xdm/xdm-config").unlink()

        result = self.run_guest("exec /usr/lib/reliefos/reliefos-xdm")

        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        log = self.log()
        self.assertIn("missing /etc/X11/xdm/xdm-config", log)
        self.assertIn("tty1 restored to text login", log)
        self.assertFalse((self.sandbox / "run/xdm.argv").exists(),
                         "XDM must not start with an incomplete standard configuration")

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
        self.assertTrue(all(item in lines for item in wanted),
                        f"missing lifecycle events {[item for item in wanted if item not in lines]}: {lines}")
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

    def test_session_still_runs_when_event_fd_is_not_in_xdm_export_list(self):
        self.install_session_stubs()
        self.install_xdm(0)
        config = self.sandbox / "etc/X11/xdm/xdm-config"
        config.write_text("\n".join(
            line for line in config.read_text().splitlines()
            if not line.lstrip().startswith("DisplayManager.exportList:")
        ) + "\n")

        result = self.run_guest("exec /usr/lib/reliefos/reliefos-xdm")

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        user_log = (self.sandbox / "home/tester/.xsession-errors").read_text()
        wanted = ["PAM authentication accepted",
                  "icewm started for uid=1000",
                  "st started",
                  "xdm session ended"]
        for event in wanted[1:]:
            self.assertIn(event, user_log)
        root_log = self.events()
        for event in wanted:
            self.assertNotIn(event, root_log)
        self.assertIn("reliefos-xdm: xdm exit status=0", root_log)
        self.assertIn("reliefos-xdm: tty1 restored to text login", root_log)
        self.assertEqual(stat.S_IMODE((self.sandbox / "var/log/xdm.log").stat().st_mode),
                         0o600)
        self.assertFalse((self.sandbox / "run/reliefos/xdm/events").exists())

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
        for script in ("usr/lib/reliefos/reliefos-xdm",
                       "usr/lib/X11/xdm/Xsession"):
            text = (self.sandbox / script).read_text()
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

    def test_fbdev_wrapper_preflights_only_without_drm_and_preserves_xorg_arguments(self):
        write_stub(self.sandbox / "usr/sbin/fbset",
                   'printf "<%s>\\n" "$@" >> /tmp/fbset.argv\nexit 1\n')
        write_stub(self.sandbox / "usr/bin/Xorg",
                   'printf "<%s>\\n" "$@" >> /tmp/xorg.argv\n'
                   'printf "Xorg stdout reached tty1\\n"\n'
                   'printf "Xorg stderr reached tty1\\n" >&2\n'
                   'exit 0\n')
        result = self.run_guest_with_fbdev(
            'exec /usr/lib/reliefos/xorg-tty-wrapper :0 -config /etc/X11/xorg.conf '
            '-nolisten tcp vt1 -keeptty "argument with spaces"')

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((self.sandbox / "tmp/fbset.argv").is_file(),
                        "fbdev fallback did not run the framebuffer preflight")
        self.assertEqual((self.sandbox / "tmp/fbset.argv").read_text().splitlines(),
                         ["<-fb>", "</dev/fb0>", "<-depth>", "<32>"])
        self.assertEqual((self.sandbox / "tmp/xorg.argv").read_text().splitlines(),
                         ["<:0>", "<-config>", "</etc/X11/xorg.conf>",
                          "<-nolisten>", "<tcp>", "<vt1>", "<-keeptty>",
                          "<argument with spaces>"])
        tty_output = (self.sandbox / "dev/tty1").read_text()
        self.assertIn("Xorg stdout reached tty1", tty_output)
        self.assertIn("Xorg stderr reached tty1", tty_output)

    def test_drm_device_skips_fbdev_preflight_and_keeps_xorg_arguments(self):
        write_stub(self.sandbox / "usr/sbin/fbset",
                   'printf "called\\n" >> /tmp/fbset.argv\n')
        write_stub(self.sandbox / "usr/bin/Xorg",
                   'printf "<%s>\\n" "$@" >> /tmp/xorg.argv\nexit 0\n')
        result = self.run_guest_with_fbdev(
            'exec /usr/lib/reliefos/xorg-tty-wrapper :0 -config /etc/X11/custom.conf '
            '-nolisten tcp vt1 -keeptty', drm=True)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((self.sandbox / "tmp/fbset.argv").exists())
        self.assertEqual((self.sandbox / "tmp/xorg.argv").read_text().splitlines(),
                         ["<:0>", "<-config>", "</etc/X11/custom.conf>",
                          "<-nolisten>", "<tcp>", "<vt1>", "<-keeptty>"])


if __name__ == "__main__":
    unittest.main()
