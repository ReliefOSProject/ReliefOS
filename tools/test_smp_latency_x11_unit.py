#!/usr/bin/env python3
"""Host Xvfb checks for the X11 client repaint probe (no guest VM)."""
import os
import re
import signal
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tools/tests/smp_x11_latency_probe.c"
WM_SOURCE = r'''
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdlib.h>
#include <unistd.h>
int main(void) {
    Display *d = XOpenDisplay(0); if (!d) return 2;
    Window root = DefaultRootWindow(d), w = XCreateSimpleWindow(d, root, 0, 0, 1, 1, 0, 0, 0);
    if (getenv("BLOCK_MAP")) XSelectInput(d, root, SubstructureRedirectMask);
    Atom a = XInternAtom(d, "_NET_SUPPORTING_WM_CHECK", False);
    XChangeProperty(d, root, a, XA_WINDOW, 32, PropModeReplace, (unsigned char *)&w, 1);
    XChangeProperty(d, w, a, XA_WINDOW, 32, PropModeReplace, (unsigned char *)&w, 1);
    XSync(d, False); write(1, "R", 1); for (;;) pause();
}
'''


class X11Probe(unittest.TestCase):
    def test_window_manager_that_never_maps_window_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary, wm, source = (Path(tmp) / name for name in ("probe", "wm", "wm.c"))
            subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror", str(SOURCE), "-lX11", "-o", str(binary)], check=True)
            source.write_text(WM_SOURCE)
            subprocess.run(["cc", str(source), "-lX11", "-o", str(wm)], check=True)
            xvfb = subprocess.Popen(["Xvfb", "-displayfd", "1", "-screen", "0", "800x600x24", "-nolisten", "tcp", "-ac"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            manager = None
            try:
                env = {**os.environ, "DISPLAY": ":" + xvfb.stdout.readline().strip()}
                manager = subprocess.Popen([str(wm)], env={**env, "BLOCK_MAP": "1"}, stdout=subprocess.PIPE)
                self.assertEqual(manager.stdout.read(1), b"R")
                ready = Path(tmp) / "ready"
                probe = subprocess.run([str(binary), "--samples", "20", "--interval-ms", "1", "--timeout-seconds", "1", "--ready-file", str(ready)], env=env, capture_output=True, timeout=5)
                self.assertNotEqual(probe.returncode, 0, probe.stdout)
                self.assertFalse(ready.exists())
            finally:
                if manager: manager.terminate(); manager.wait(timeout=5); manager.stdout.close()
                xvfb.terminate(); xvfb.wait(timeout=5); xvfb.stdout.close(); xvfb.stderr.close()

    def test_repaint_samples_require_wm_and_preserve_each_latency(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary, wm, wm_source = (Path(tmp) / x for x in ("probe", "wm", "wm.c"))
            build = subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror", str(SOURCE),
                                    "-lX11", "-o", str(binary)], text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            wm_source.write_text(WM_SOURCE)
            subprocess.run(["cc", str(wm_source), "-lX11", "-o", str(wm)], check=True)
            xvfb = subprocess.Popen(["Xvfb", "-displayfd", "1", "-screen", "0", "800x600x24",
                                     "-nolisten", "tcp", "-ac"], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    text=True)
            manager = None
            try:
                display = ":" + xvfb.stdout.readline().strip()
                env = {**os.environ, "DISPLAY": display}
                no_wm = subprocess.run([str(binary), "--samples", "20", "--interval-ms", "1"],
                                       env=env, capture_output=True, timeout=10)
                self.assertNotEqual(no_wm.returncode, 0)
                manager = subprocess.Popen([str(wm)], env=env, stdout=subprocess.PIPE)
                self.assertEqual(manager.stdout.read(1), b"R")
                result = subprocess.run([str(binary), "--samples", "20", "--interval-ms", "1"],
                                        env=env, text=True, capture_output=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertRegex(result.stdout, r"X11 count=20 p50_ns=\d+ p95_ns=\d+ p99_ns=\d+ max_ns=\d+ wm=1 pixels=1")
                self.assertEqual(len(re.findall(r"^X11_SAMPLE ns=\d+$", result.stdout, re.M)), 20)
                ready, stop = Path(tmp) / "ready", Path(tmp) / "stop"
                probe = subprocess.Popen([str(binary), "--ready-file", str(ready), "--stop-file", str(stop),
                                          "--samples", "1000", "--interval-ms", "1"],
                                         env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                try:
                    deadline = time.monotonic() + 5
                    while not ready.exists() and time.monotonic() < deadline:
                        time.sleep(0.005)
                    self.assertTrue(ready.exists(), "probe did not acknowledge readiness")
                    time.sleep(0.05)
                    stop.touch()
                    output, error = probe.communicate(timeout=5)
                    self.assertEqual(probe.returncode, 0, error)
                    self.assertGreaterEqual(len(re.findall(r"^X11_SAMPLE ns=\d+$", output, re.M)), 10)
                finally:
                    if probe.poll() is None:
                        probe.terminate(); probe.wait(timeout=5)
                os.kill(xvfb.pid, signal.SIGSTOP)
                try:
                    hung = subprocess.run([str(binary), "--timeout-seconds", "1"], env=env,
                                          capture_output=True, timeout=5)
                    self.assertEqual(hung.returncode, -signal.SIGALRM)
                finally:
                    os.kill(xvfb.pid, signal.SIGCONT)
            finally:
                if manager:
                    manager.terminate(); manager.wait(timeout=5)
                    manager.stdout.close()
                xvfb.terminate(); xvfb.wait(timeout=5)
                xvfb.stdout.close(); xvfb.stderr.close()


if __name__ == "__main__":
    unittest.main()
