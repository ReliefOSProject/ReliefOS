#!/usr/bin/env python3
"""Check that ReliefOS supervise-daemon services stop within a bounded schedule."""

from pathlib import Path
from pathlib import PurePosixPath
import re
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[1]
INIT_DIR = ROOT / "system/rootfs/etc/init.d"
EXPECTED_RETRY = "TERM/1/KILL/1"


class OpenRCShutdownTests(unittest.TestCase):
    def test_runtime_keeps_legacy_ipc_socket_parent_available(self) -> None:
        ipc_header = (ROOT / "userland/runtime/include/reliefos/unix_ipc.h").read_text(
            encoding="utf-8",
        )
        socket_paths = re.findall(
            r'^#define RELIEFOS_IPC_SOCK_\w+ "([^"]+)"$', ipc_header, re.MULTILINE,
        )
        self.assertTrue(socket_paths)
        self.assertEqual({"/run/leonos"}, {
            str(PurePosixPath(path).parent) for path in socket_paths
        })

        runtime_script = (INIT_DIR / "reliefos-runtime").read_text(encoding="utf-8")
        self.assertLess(
            runtime_script.index("checkpath"),
            runtime_script.index("ln -s /run/reliefos /run/leonos"),
        )
        self.assertIn("ln -s /run/reliefos /run/leonos || return 1", runtime_script)
        self.assertIn("test -d /run/leonos || return 1", runtime_script)

    def test_reliefos_services_have_one_canonical_runlevel_entry(self) -> None:
        services = ("device", "dhcp", "ntp")
        for service in services:
            canonical = "reliefos-" + service
            legacy = "leonos-" + service
            with self.subTest(service=service):
                self.assertTrue((INIT_DIR / canonical).is_file())
                self.assertTrue((INIT_DIR / legacy).is_file())
                self.assertEqual(
                    "../../init.d/" + canonical,
                    (ROOT / "system/rootfs/etc/runlevels/default" / canonical).readlink().as_posix(),
                )
                self.assertEqual(
                    "../../init.d/" + canonical,
                    (ROOT / "system/rootfs/etc/runlevels/installer" / canonical).readlink().as_posix(),
                )
                for level in ("default", "installer"):
                    links = [path for path in (ROOT / "system/rootfs/etc/runlevels" / level).iterdir()
                             if path.is_symlink() and path.readlink().as_posix() == "../../init.d/" + canonical]
                    self.assertEqual(1, len(links), level + "/" + canonical)
                self.assertFalse((ROOT / "system/rootfs/etc/runlevels/default" / legacy).exists())
                self.assertFalse((ROOT / "system/rootfs/etc/runlevels/installer" / legacy).exists())
                wrapper = (INIT_DIR / legacy).read_text(encoding="utf-8")
                self.assertIn("reliefos-" + service, wrapper)

        runtime = "reliefos-runtime"
        self.assertTrue((INIT_DIR / runtime).is_file())
        self.assertEqual(
            "../../init.d/" + runtime,
            (ROOT / "system/rootfs/etc/runlevels/sysinit" / runtime).readlink().as_posix(),
        )
        self.assertFalse((ROOT / "system/rootfs/etc/runlevels/sysinit/leonos-runtime").exists())
        runtime_script = (INIT_DIR / runtime).read_text(encoding="utf-8")
        self.assertLess(runtime_script.index("reliefos-migrate"), runtime_script.index("checkpath"))
        self.assertTrue((ROOT / "system/rootfs/etc/pam.d/leonos-gui").is_symlink())
        self.assertEqual("reliefos-gui", (ROOT / "system/rootfs/etc/pam.d/leonos-gui").readlink().as_posix())

    def test_supervised_services_have_bounded_stop_schedule(self) -> None:
        services = sorted(INIT_DIR.iterdir())
        supervised = []
        for path in services:
            if not path.is_file():
                continue
            text = path.read_text(encoding="utf-8")
            if "supervisor=supervise-daemon" not in text:
                continue
            supervised.append(path.name)
            match = re.search(r"^retry=(\S+)$", text, re.MULTILINE)
            self.assertIsNotNone(match, path.name)
            self.assertEqual(EXPECTED_RETRY, match.group(1), path.name)
        self.assertGreaterEqual(len(supervised), 1)

    def test_init_scripts_are_shell_syntax_valid(self) -> None:
        for path in sorted(INIT_DIR.iterdir()):
            if not path.is_file():
                continue
            text = path.read_text(encoding="utf-8")
            if text.startswith("#!"):
                self.assertEqual(0, subprocess.run(
                    ["sh", "-n", str(path)], check=False,
                ).returncode, path.name)


if __name__ == "__main__":
    unittest.main()
