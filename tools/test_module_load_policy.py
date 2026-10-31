#!/usr/bin/env python3
"""Exercise static-config precedence and modalias selection without host module calls."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
POLICY = ROOT / "system/rootfs/usr/lib/reliefos/module-policy"


class Policy(unittest.TestCase):
    def test_platform_defaults_follow_running_release_and_admin_override(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-versioned-module-policy-") as directory:
            root = Path(directory)
            for release, text in (("old", "serial\n"), ("new", "mouse\n")):
                path = root / "lib/modules" / release / "modules-load.d/reliefnt-platform.conf"
                path.parent.mkdir(parents=True)
                path.write_text(text)
            script = 'set -eu; . "$1"; modprobe() { printf "%s\\n" "$2"; }; module_config_load "$2" "$3"'
            def load(release):
                return subprocess.check_output(["sh", "-c", script, "policy", str(POLICY), str(root), release], text=True).splitlines()
            self.assertEqual(load("old"), ["serial"])
            self.assertEqual(load("new"), ["mouse"])
            override = root / "etc/modules-load.d/reliefnt-platform.conf"
            override.parent.mkdir(parents=True)
            override.write_text("admin\n")
            self.assertEqual(load("old"), ["admin"])
            override.unlink()
            override.symlink_to("/dev/null")
            self.assertEqual(load("old"), [])

    def test_config_precedence_blacklist_flags_and_matching_device_only(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-module-policy-") as directory:
            root = Path(directory)
            for path, text in {"usr/lib/modules-load.d/10-base.conf": "vendor\n",
                               "etc/modules-load.d/10-base.conf": "# override\nadmin # safe comment\n",
                               "usr/lib/modules-load.d/20-mask.conf": "masked\n",
                               "usr/lib/modules-load.d/30-platform.conf": "mouse\nserial\n",
                               "lib/modules/native/modules.alias": "alias pci:v00008086d0000100E* e1000\nalias pci:v00001234* absent\n",
                               "sys/bus/pci/devices/0000:00:02.0/modalias": "pci:v00008086d0000100Esv00000000\n"}.items():
                target = root / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(text)
            (root / "etc/modules-load.d/20-mask.conf").symlink_to("/dev/null")
            log = root / "calls"
            script = 'set -eu; . "$1"; modprobe() { printf "%s %s\\n" "$1" "$2" >>"$LOG"; }; module_config_load "$2"; module_coldplug "$2" native'
            subprocess.run(["sh", "-c", script, "policy", str(POLICY), str(root)],
                           env={**os.environ, "LOG": str(log)}, check=True)
            self.assertEqual(log.read_text().splitlines(), ["-b admin", "-b mouse", "-b serial", "-b e1000"])
            (root / "etc/modules-load.d/00-invalid.conf").write_text("bad option=belongs-in-modprobe.d\n")
            result = subprocess.run(["sh", "-c", script, "policy", str(POLICY), str(root)],
                                    env={**os.environ, "LOG": str(log)}, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(log.read_text().splitlines(), ["-b admin", "-b mouse", "-b serial", "-b e1000"])

    def test_entrypoint_refuses_host_and_service_order(self):
        result = subprocess.run(["sh", str(ROOT / "system/rootfs/usr/lib/reliefos/modules-load")], capture_output=True)
        self.assertEqual(result.returncode, 77)
        self.assertIn(b"refusing host", result.stderr)
        link = ROOT / "system/rootfs/etc/runlevels/sysinit/reliefos-modules"
        self.assertEqual(os.readlink(link), "../../init.d/reliefos-modules")
        service = (ROOT / "system/rootfs/etc/init.d/reliefos-modules").read_text()
        self.assertIn("need reliefos-runtime", service)
        self.assertIn("before reliefos-device reliefos-audio reliefos-dhcp", service)

    def test_openrc_virtual_providers_do_not_shadow_real_hwdrivers(self):
        # Alpine OpenRC ships a real hwdrivers service. The native module
        # loader has its own explicit dependency name; claiming that real name
        # as a virtual provider makes rc-depend reject the service graph.
        service = ROOT / "system/rootfs/etc/init.d/reliefos-modules"
        script = 'need() { :; }; before() { :; }; provide() { printf "%s\\n" "$@"; }; . "$1"; depend'
        providers = subprocess.check_output(["sh", "-c", script, "policy", str(service)], text=True).splitlines()
        self.assertNotIn("hwdrivers", providers)


if __name__ == "__main__":
    unittest.main()
