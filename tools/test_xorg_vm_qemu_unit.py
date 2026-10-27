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
        self.assertEqual(classify_greeter_text("ReliefOS st\n$"), "session")


class EvidenceTests(unittest.TestCase):
    def test_upstream_default_gray_xdm_on_full_hd_display_is_detected(self):
        frame = Image.new("RGB", (1920, 1080), "black")
        draw = ImageDraw.Draw(frame)
        draw.rectangle((643, 279, 1282, 529), fill=(191, 191, 191))
        draw.rectangle((1158, 304, 1271, 503), fill="white")
        draw.line((1158, 304, 1271, 503), fill="black", width=30)
        draw.line((1271, 304, 1158, 503), fill="black", width=30)
        draw.rectangle((771, 383, 1101, 409), outline="black")
        rectangle = _bright_panel_bbox(frame)
        self.assertIsNotNone(rectangle)
        left, top, right, bottom = rectangle
        self.assertLessEqual(left, 713)
        self.assertLessEqual(top, 363)
        self.assertGreaterEqual(right, 1102)
        self.assertGreaterEqual(bottom, 447)

    def test_session_st_is_not_an_xlogin_panel(self):
        frame = Image.new("RGB", (1280, 800), "black")
        ImageDraw.Draw(frame).rectangle((0, 20, 605, 440), fill="white")
        self.assertIsNone(_bright_panel_bbox(frame))

    def test_centered_xlogin_panel_is_detected(self):
        frame = Image.new("RGB", (1280, 800), "black")
        ImageDraw.Draw(frame).rectangle((271, 157, 1010, 489), fill="white")
        self.assertEqual(_bright_panel_bbox(frame), (271, 157, 1011, 490))

    def test_centered_neutral_gray_xlogin_panel_is_detected(self):
        frame = Image.new("RGB", (1280, 800), "black")
        ImageDraw.Draw(frame).rectangle((271, 157, 1010, 489), fill=(190, 198, 205))
        self.assertEqual(_bright_panel_bbox(frame), (271, 157, 1011, 490))

    def test_gray_panel_survives_input_box_and_password_label_gap(self):
        frame = Image.new("RGB", (1280, 800), "black")
        draw = ImageDraw.Draw(frame)
        draw.rectangle((323, 242, 837, 435), fill=(190, 198, 205))
        # Model the dark input outline and a 9-row text glyph at the panel's
        # near-threshold width.  The label makes every row in its height miss
        # the detector's 40%-of-screen bright-row threshold.
        draw.rectangle((456, 310, 780, 335), outline=(20, 20, 20), width=1)
        label = (20, 20, 20)
        draw.rectangle((456, 340, 459, 348), fill=label)
        draw.rectangle((456, 340, 466, 343), fill=label)
        draw.rectangle((456, 344, 466, 347), fill=label)
        draw.rectangle((463, 340, 466, 345), fill=label)
        self.assertEqual(_bright_panel_bbox(frame), (323, 242, 838, 436))

    def test_neutral_gray_terminal_touching_display_edge_is_not_xlogin(self):
        frame = Image.new("RGB", (1280, 800), "black")
        ImageDraw.Draw(frame).rectangle((0, 20, 605, 440), fill=(190, 198, 205))
        self.assertIsNone(_bright_panel_bbox(frame))

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
            "icewm started for uid=1000",
            "st started",
            "xdm session ended",
            "tty1 restored to text login",
        ])
        self.assertEqual(lifecycle_events(log), [
            "PAM authentication accepted",
            "icewm started for uid",
            "st started",
            "xdm session ended",
            "tty1 restored to text login",
        ])
        require_event_order(log)

    def test_lifecycle_order_rejects_missing_or_root_icewm(self):
        root_log = "PAM authentication accepted\nicewm started for uid=0\nst started\n"
        with self.assertRaises(AssertionError):
            require_event_order(root_log)
        missing = "PAM authentication accepted\nst started\n"
        with self.assertRaises(AssertionError):
            require_event_order(missing)


if __name__ == "__main__":
    unittest.main()
