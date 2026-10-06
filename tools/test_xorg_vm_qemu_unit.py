#!/usr/bin/env python3
"""Unit checks for evidence parsing in test_xorg_vm_qemu.py.

These tests deliberately avoid starting QEMU.  They cover the failure modes
where a broad OCR result or an unrelated log line could make the acceptance
script claim progress that was never observed.
"""
import sys
import unittest
from pathlib import Path
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))

from test_xorg_vm_qemu import (
    classify_greeter_text,
    lifecycle_events,
    require_event_order,
    require_vt_handshake,
    _bright_panel_bbox,
)


class GreeterStageTests(unittest.TestCase):
    def test_password_label_survives_ocr_noise(self):
        text = "This is an unsecure session\nLogin: test\nPassword: H\n"
        self.assertEqual(classify_greeter_text(text), "password")

    def test_login_stage_is_distinct_from_password_stage(self):
        text = "This is an unsecure session\nLogin: | |\n"
        self.assertEqual(classify_greeter_text(text), "login")

    def test_login_label_does_not_require_legacy_banner(self):
        self.assertEqual(classify_greeter_text("X Window System\nLogin: E\n"), "login")

    def test_missing_banner_cannot_count_as_greeter(self):
        self.assertEqual(classify_greeter_text("ReliefOS xterm\n$"), "session")


class EvidenceTests(unittest.TestCase):
    def test_session_xterm_is_not_an_xlogin_panel(self):
        frame = Image.new("RGB", (1280, 800), "black")
        ImageDraw.Draw(frame).rectangle((0, 20, 605, 440), fill="white")
        self.assertIsNone(_bright_panel_bbox(frame))

    def test_centered_xlogin_panel_is_detected(self):
        frame = Image.new("RGB", (1280, 800), "black")
        ImageDraw.Draw(frame).rectangle((271, 157, 1010, 489), fill="white")
        self.assertEqual(_bright_panel_bbox(frame), (271, 157, 1011, 490))

    def test_vt_evidence_requires_process_handshake(self):
        trace = "\n".join([
            "VT mode vt=1 mode=1 relsig=10 acqsig=12",
            "VT graphics vt=1 enabled=1",
            "VT switch request 1 -> 2",
            "VT release signal sent for vt=1",
            "VT active 2 graphics=0",
            "VT switch request 2 -> 1",
            "VT active 1 graphics=1",
            "VT acquire signal sent for vt=1",
        ])
        require_vt_handshake(trace)
        with self.assertRaises(AssertionError):
            require_vt_handshake("VT active 2 graphics=0")

    def test_lifecycle_events_require_real_order(self):
        log = "\n".join([
            "PAM authentication accepted",
            "twm started for uid=1000",
            "xterm started",
            "xdm session ended",
            "tty1 restored to text login",
        ])
        self.assertEqual(lifecycle_events(log), [
            "PAM authentication accepted",
            "twm started for uid",
            "xterm started",
            "xdm session ended",
            "tty1 restored to text login",
        ])
        require_event_order(log)

    def test_lifecycle_order_rejects_missing_or_root_twm(self):
        root_log = "PAM authentication accepted\ntwm started for uid=0\nxterm started\n"
        with self.assertRaises(AssertionError):
            require_event_order(root_log)
        missing = "PAM authentication accepted\nxterm started\n"
        with self.assertRaises(AssertionError):
            require_event_order(missing)


if __name__ == "__main__":
    unittest.main()
